// sph_fluid_pool: port of fluids5.0 (Rama Hoetzlein, http://fluids3.com), the
// SPH "wave pool". The fluid is simulated by Slang kernels on the nvrhi CUDA
// device (sph::ISPHParticles = data, sph::ISPHSolver = operations) and
// rasterized as points by nvrhi from vertex buffers shared with the CUDA
// device (the CUDA / OpenGL interop buffers of the reference).
//
//   sph_fluid_pool [--particles N] [--example 0..3] [--paused] [--check N]
//                  [--screenshot file.png [--frames N]] [-d3d12 | -vk] [--debug]
//
// --check N reads the particles back after simulation step N and logs a sanity
// report (finite values, bounding box against the domain, speed and density range).
//
// Frame (GVDBApp::Render): OnRender records one simulation step into the
// compute context between the presenter's beginComputeFrame / endComputeFrame:
// the CUDA queue waits for the previous frame's graphics work, the graphics
// queue for the step. OnDrawOverlay then clears an own colour + depth target,
// draws the particles and the ground grid into it and blits it to the back
// buffer. D3D11 cannot share the vertex buffers and is refused (GVDBApp).
#include "SPHFluid.h"
#include "SPHFluidScene.h"
#include <sample-utils/GVDBApp.h>
#include <donut/core/log.h>
#include <nvrhi/utils.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace donut;
using namespace SampleUtils;

namespace {

struct SPHFluidPoolOptions {
    uint32_t maxParticles = 4000000;   // Restart(4000000) of the reference
    int example = int(sph::SPHExample::WavePool);
    bool paused = false;
    uint32_t checkStep = 0;   // --check N: sanity report after step N (0 = off)
};

// Camera3D::setFov(80) of the reference, taken literally: an 80 degree
// horizontal field of view at the window's aspect ratio (square pixels).
// Note that the reference executable shows a narrower view than this: its
// projection matrix (Camera3D::updateMatricies) uses 2*near/sx instead of
// near/sx and a fixed 800/600 aspect, which gives 45.5 degrees horizontally
// and 34.9 vertically, stretched over the window.
constexpr float kReferenceFovDeg = 80.f;
// Camera3D dolly of the reference's setOrbit(angles, target, 1000, 70): only used by the zoom formula.
constexpr float kReferenceDolly = 70.f;

struct LinesConstants {   // sample-utils/shaders/Lines.hlsl
    dm::float4x4 matViewProj;
};
struct LineVertex {
    dm::float3 position;
    dm::float4 color;
};

}  // namespace

class SPHFluidPoolApp : public GVDBApp {
    NVRHI_INHERIT_INTERFACE_TABLE()
 public:
    SPHFluidPoolApp(app::DeviceManager *deviceManager, const SPHFluidPoolOptions &options)
        : GVDBApp(deviceManager), m_options(options), m_paused(options.paused) {}

    ~SPHFluidPoolApp() override {
        if (m_fps.y > 0.f)
            log::info("Compute: %4.0f FPS (%4.2f msec) as the reference averages it; mean step %.2f msec, last %.2f msec, "
                      "%d timed steps, %u particles",
                      m_fps.z, 1000.0f / m_fps.z, m_elapsedSum / m_fps.y, m_elapsed, int(m_fps.y),
                      m_particles ? m_particles->getNumParticles() : 0u);
        if (m_frames > 1) {
            const float seconds = std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - m_firstFrameTime).count();
            const float frameMs = 1000.f * seconds / float(m_frames - 1);
            const float waitMs = m_waitSum / float(m_frames);   // previous frame's rasterization and presentation
            log::info("Frames: %u, %.2f msec per frame (%.2f waiting for the previous frame before the timed step); "
                      "vertex buffers shared with the CUDA device",
                      m_frames, frameMs, waitMs);
        }
        syncCompute(getCompute());
        if (GetDevice()) GetDevice()->waitForIdle();
    }

    bool OnInit() override {
        log::info("Starting particles:");
        if (NVRHI_FAILED(sph::createSPHParticles(getCompute(), &m_particles)) || !m_particles) {
            log::error("createSPHParticles failed");
            return false;
        }
        if (!restart()) return false;
        if (NVRHI_FAILED(sph::createSPHSolver(m_particles, &m_solver)) || !m_solver) {
            log::error("createSPHSolver failed");
            return false;
        }

        // ---- scene: the particle set under its own node (identity: fluid units = scene units) ----
        m_instance = MAKE_RC_OBJ_PTR(sph::SPHFluidInstance, m_particles.Get());
        getSceneGraph()->AttachLeafNode(getSceneGraph()->GetRootNode(), m_instance)->SetName("fluid");

        // ---- camera (Sample::init of the reference) ----
        getCamera()->zNear = 1.f;
        getCamera()->zFar = 10000.f;
        getCameraOrbit().setOrbit({40.f, 50.f, 0.f}, {250.f, 0.f, 250.f}, 1000.f);
        getLightOrbit().setOrbit({40.f, 50.f, 0.f}, {250.f, 0.f, 250.f}, 1000.f);

        m_pointRenderer = std::make_unique<sph::SPHPointRenderer>(GetDevice(), getShaderFactory());
        if (!createPipelines()) return false;
        createRasterTargets(getPresenter()->getWidth(0), getPresenter()->getHeight(0));
        return m_rasterFramebuffer != nullptr;
    }

    // Particles::Restart of the reference: default + example parameters,
    // particle buffers, acceleration grid, the initial block of particles,
    // upload. The particle buffers are allocated once (the capacity does not
    // change between restarts).
    bool restart() {
        SPHFluidParams params;
        sph::setupDefaultParams(params);
        params.pnum = int(m_options.maxParticles);   // the regression example sizes its domain from it
        sph::setupExampleParams(params, sph::SPHExample(m_options.example));

        if (m_particles->getMaxParticles() != m_options.maxParticles) {
            sph::SPHParticlesDesc desc;
            desc.maxParticles = m_options.maxParticles;
            desc.interop = getInteropDevice();
            nvrhi::FRESULT fr = m_particles->configure(desc);
            if (NVRHI_FAILED(fr)) {
                log::error("Cannot allocate the particle buffers for %u particles on %s (error %d). The Position / Color / "
                           "Velocity buffers are vertex buffers imported into the CUDA device.",
                           m_options.maxParticles, nvrhi::utils::GraphicsAPIToString(GetDevice()->getGraphicsAPI()),
                           int(fr));
                return false;
            }
        }
        m_particles->setParams(params);
        if (NVRHI_FAILED(m_particles->rebuildAccelGrid())) {
            log::error("Cannot build the acceleration grid");
            return false;
        }
        m_particles->clear();
        m_particles->addPointsInVolume(params.init_min, params.init_max);

        // Initial transfer.
        nvrhi::FRESULT fr = m_particles->commit();
        if (NVRHI_FAILED(fr)) {
            log::error("Cannot upload the particles (error %d)", int(fr));
            return false;
        }
        if (m_solver) m_solver->reset();
        m_fps = dm::float3(0.f);
        m_elapsedSum = 0.f;
        log::info("# Particles:  %u (max %u), example %d", m_particles->getNumParticles(), m_particles->getMaxParticles(),
                  m_options.example);
        return true;
    }

    bool createPipelines() {
        auto *sf = getShaderFactory();
        // Ground grid: the framework's line shader with the depth test of the raster target.
        {
            m_linesVS = sf->CreateShader("Lines.hlsl", "VSMain", nullptr, nvrhi::ShaderType::Vertex);
            m_linesPS = sf->CreateShader("Lines.hlsl", "PSMain", nullptr, nvrhi::ShaderType::Pixel);
            if (!m_linesVS || !m_linesPS) {
                log::error("Lines.hlsl shaders not found");
                return false;
            }
            nvrhi::VertexAttributeDesc attributes[] = {
                nvrhi::VertexAttributeDesc()
                    .setName("POSITION")
                    .setFormat(nvrhi::Format::RGB32_FLOAT)
                    .setOffset(offsetof(LineVertex, position))
                    .setBufferIndex(0)
                    .setElementStride(sizeof(LineVertex)),
                nvrhi::VertexAttributeDesc()
                    .setName("COLOR")
                    .setFormat(nvrhi::Format::RGBA32_FLOAT)
                    .setOffset(offsetof(LineVertex, color))
                    .setBufferIndex(0)
                    .setElementStride(sizeof(LineVertex)),
            };
            GetDevice()->createInputLayout(attributes, 2, m_linesVS, &m_linesInputLayout);

            GetDevice()->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(LinesConstants), "grid constants", 16),
                                      &m_linesConstants);
            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.visibility = nvrhi::ShaderType::Vertex;
            layoutDesc.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0)};
            GetDevice()->createBindingLayout(layoutDesc, &m_linesLayout);
            nvrhi::BindingSetDesc setDesc;
            setDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_linesConstants)};
            GetDevice()->createBindingSet(setDesc, m_linesLayout, &m_linesSet);
            if (!m_linesInputLayout || !m_linesConstants || !m_linesLayout || !m_linesSet) return false;

            // Sample::display: lines every 50 units from 0 to 500, just below the floor.
            std::vector<LineVertex> vertices;
            const dm::float4 color(.2f, .2f, .2f, 1.f);
            for (int i = 0; i <= 500; i += 50) {
                vertices.push_back({{float(i), -0.01f, 0.f}, color});
                vertices.push_back({{float(i), -0.01f, 500.f}, color});
                vertices.push_back({{0.f, -0.01f, float(i)}, color});
                vertices.push_back({{500.f, -0.01f, float(i)}, color});
            }
            m_linesVertexCount = uint32_t(vertices.size());
            nvrhi::BufferDesc desc;
            desc.byteSize = vertices.size() * sizeof(LineVertex);
            desc.isVertexBuffer = true;
            desc.initialState = nvrhi::ResourceStates::VertexBuffer;
            desc.keepInitialState = true;
            desc.debugName = "grid vertices";
            GetDevice()->createBuffer(desc, &m_linesVertices);
            if (!m_linesVertices) return false;
            nvrhi::CommandListHandle uploadList;
            GetDevice()->createCommandList(nvrhi::CommandListParameters(), &uploadList);
            uploadList->open();
            uploadList->writeBuffer(m_linesVertices, vertices.data(), desc.byteSize);
            uploadList->close();
            GetDevice()->executeCommandList(uploadList);
        }
        // Presentation of the raster target (FullScreen.hlsl, one texture).
        {
            auto vs = sf->CreateShader("FullScreen.hlsl", "VSMain", nullptr, nvrhi::ShaderType::Vertex);
            std::vector<engine::ShaderMacro> defines = {{"HAS_TEXTURE_2", "0"}};
            auto ps = sf->CreateShader("FullScreen.hlsl", "PSMain", &defines, nvrhi::ShaderType::Pixel);
            if (!vs || !ps) return false;

            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
            layoutDesc.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(dm::float4)),
                                   nvrhi::BindingLayoutItem::Sampler(0), nvrhi::BindingLayoutItem::Texture_SRV(0)};
            GetDevice()->createBindingLayout(layoutDesc, &m_presentLayout);

            nvrhi::SamplerDesc samplerDesc;
            samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::ClampToEdge);
            samplerDesc.setAllFilters(false);
            GetDevice()->createSampler(samplerDesc, &m_sampler);

            nvrhi::GraphicsPipelineDesc psoDesc;
            psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
            psoDesc.VS = vs;
            psoDesc.PS = ps;
            psoDesc.bindingLayouts = {m_presentLayout};
            psoDesc.renderState.depthStencilState.depthTestEnable = false;
            psoDesc.renderState.depthStencilState.depthWriteEnable = false;
            psoDesc.renderState.rasterState.setCullNone();
            GetDevice()->createGraphicsPipeline1(psoDesc, GetDeviceManager()->GetFramebuffer(0)->getFramebufferInfo().getInfo(),
                                                 &m_presentPipeline);
            if (!m_presentPipeline) return false;
        }
        return true;
    }

    void createRasterTargets(uint32_t width, uint32_t height) {
        width = std::max(width, 1u);
        height = std::max(height, 1u);
        GetDevice()->waitForIdle();
        m_presentSet = nullptr;
        m_rasterFramebuffer = nullptr;
        m_linesPipeline = nullptr;
        m_colorTexture = nullptr;
        m_depthTexture = nullptr;

        nvrhi::TextureDesc texDesc;
        texDesc.dimension = nvrhi::TextureDimension::Texture2D;
        texDesc.width = width;
        texDesc.height = height;
        texDesc.format = nvrhi::Format::RGBA8_UNORM;
        texDesc.isShaderResource = true;
        texDesc.isRenderTarget = true;
        texDesc.initialState = nvrhi::ResourceStates::RenderTarget;
        texDesc.keepInitialState = true;
        texDesc.clearValue = nvrhi::Color(0.f, 0.f, 0.f, 1.f);
        texDesc.useClearValue = true;
        texDesc.debugName = "fluid colour";
        GetDevice()->createTexture(texDesc, &m_colorTexture);

        texDesc = nvrhi::TextureDesc();
        texDesc.dimension = nvrhi::TextureDimension::Texture2D;
        texDesc.width = width;
        texDesc.height = height;
        texDesc.format = nvrhi::Format::D24S8;
        texDesc.isTypeless = true;   // as depth-map: D3D11 needs a typeless depth resource
        texDesc.isRenderTarget = true;
        texDesc.initialState = nvrhi::ResourceStates::DepthWrite;
        texDesc.keepInitialState = true;
        texDesc.clearValue = nvrhi::Color(1.f);
        texDesc.useClearValue = true;
        texDesc.debugName = "fluid depth";
        GetDevice()->createTexture(texDesc, &m_depthTexture);
        if (!m_colorTexture || !m_depthTexture) {
            log::error("Cannot create the %ux%u raster targets", width, height);
            return;
        }

        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(m_colorTexture);
        fbDesc.setDepthAttachment(m_depthTexture);
        GetDevice()->createFramebuffer(fbDesc, &m_rasterFramebuffer);
        if (!m_rasterFramebuffer) return;

        // Grid pipeline for this framebuffer: depth tested like the 3D lines of the reference.
        {
            nvrhi::GraphicsPipelineDesc psoDesc;
            psoDesc.primType = nvrhi::PrimitiveType::LineList;
            psoDesc.VS = m_linesVS;
            psoDesc.PS = m_linesPS;
            psoDesc.inputLayout = m_linesInputLayout;
            psoDesc.bindingLayouts = {m_linesLayout};
            psoDesc.renderState.rasterState.setCullNone();
            psoDesc.renderState.depthStencilState.depthTestEnable = true;
            psoDesc.renderState.depthStencilState.depthWriteEnable = true;
            psoDesc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::Less;
            psoDesc.renderState.depthStencilState.stencilEnable = false;
            GetDevice()->createGraphicsPipeline1(psoDesc, m_rasterFramebuffer->getFramebufferInfo().getInfo(), &m_linesPipeline);
        }

        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(dm::float4)),
                            nvrhi::BindingSetItem::Sampler(0, m_sampler),
                            nvrhi::BindingSetItem::Texture_SRV(0, m_colorTexture)};
        GetDevice()->createBindingSet(setDesc, m_presentLayout, &m_presentSet);
    }

    // --check: positions, velocities and densities read back through ISPHParticles::retrieve.
    void sanityCheck() {
        const uint32_t count = m_particles->getNumParticles();
        std::vector<dm::float3> pos(count), vel(count);
        std::vector<float> dens(count);
        if (NVRHI_FAILED(m_particles->retrieve(sph::ParticleBuffer::Position, 0, count, pos.data())) ||
            NVRHI_FAILED(m_particles->retrieve(sph::ParticleBuffer::Velocity, 0, count, vel.data())) ||
            NVRHI_FAILED(m_particles->retrieve(sph::ParticleBuffer::Density, 0, count, dens.data()))) {
            log::warning("CHECK step %u: retrieve failed", m_solver->getFrame());
            return;
        }
        const SPHFluidParams &p = m_particles->getParams();
        // The boundaries are penalty forces acting within one particle radius, so allow that much penetration.
        const float tolerance = p.pradius / p.sim_scale;
        const dm::box3 domain = m_particles->getBounds();
        uint32_t nonFinite = 0, outside = 0, parked = 0, belowFloor = 0;
        dm::float3 bmin(1e30f), bmax(-1e30f);
        double speedSum = 0.0, densSum = 0.0, ySum = 0.0;
        float speedMax = 0.f, densMin = 1e30f, densMax = 0.f;
        for (uint32_t i = 0; i < count; ++i) {
            const dm::float3 &q = pos[i];
            const dm::float3 &v = vel[i];
            if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(v.x) ||
                !std::isfinite(v.y) || !std::isfinite(v.z) || !std::isfinite(dens[i])) {
                nonFinite++;
                continue;
            }
            if (q.x == -1000.f && q.y == -1000.f && q.z == -1000.f) {   // left the grid: parked by advanceParticles
                parked++;
                continue;
            }
            bmin = dm::min(bmin, q);
            bmax = dm::max(bmax, q);
            if (dm::any(q < domain.m_mins - tolerance) || dm::any(q > domain.m_maxs + tolerance)) outside++;
            if (q.y < domain.m_mins.y + (q.x - domain.m_mins.x) * p.bound_slope - tolerance) belowFloor++;
            const float speed = dm::length(v);
            speedSum += speed;
            speedMax = std::max(speedMax, speed);
            densSum += dens[i];
            densMin = std::min(densMin, dens[i]);
            densMax = std::max(densMax, dens[i]);
            ySum += q.y;
        }
        const double live = double(std::max(count - nonFinite - parked, 1u));
        log::info("CHECK step %u (t = %.3f s), %u particles: non-finite %u, parked outside the grid %u, outside the domain "
                  "(+/- %.2f) %u, below the sloped floor %u",
                  m_solver->getFrame(), m_solver->getTime(), count, nonFinite, parked, tolerance, outside, belowFloor);
        log::info("CHECK bounds (%.2f, %.2f, %.2f) .. (%.2f, %.2f, %.2f), domain (%.0f, %.0f, %.0f) .. (%.0f, %.0f, %.0f), "
                  "mean height %.2f",
                  bmin.x, bmin.y, bmin.z, bmax.x, bmax.y, bmax.z, domain.m_mins.x, domain.m_mins.y, domain.m_mins.z,
                  domain.m_maxs.x, domain.m_maxs.y, domain.m_maxs.z, ySum / live);
        log::info("CHECK speed mean %.3f max %.3f m/s (limit %.0f), density mean %.1f min %.1f max %.1f (rest %.0f)",
                  speedSum / live, speedMax, p.VL, densSum / live, densMin, densMax, p.prest_dens);
    }

    void OnResize(uint32_t width, uint32_t height) override { createRasterTargets(width, height); }

    // CUDA side of the frame: Sample::display's "Run fluid simulation!".
    void OnRender(uint32_t width, uint32_t height) override {
        if (!m_rasterFramebuffer || m_colorTexture->getDesc().width != width || m_colorTexture->getDesc().height != height)
            createRasterTargets(width, height);

        if (m_frames++ == 0) m_firstFrameTime = std::chrono::high_resolution_clock::now();

        getCamera()->verticalFov =
            verticalFovFromHorizontal(dm::radians(kReferenceFovDeg), float(width) / float(std::max(height, 1u)));
        updateScene();

        if (m_resetRequested) {
            m_resetRequested = false;
            log::info("Starting particles:");
            // The previous frame's draw may still read the shared buffers.
            syncCompute(getCompute());
            GetDevice()->waitForIdle();
            if (!restart()) log::error("Reset failed");
        }

        if (!m_paused) {
            // The Inspector shows the compute time: wait for the CUDA device to measure
            // the step (what the reference's cuCtxSynchronize does every frame).
            // Without a visible Inspector the step is only recorded.
            const bool timing = m_measureTiming && (getFrameCounter() - m_inspectorFrame) <= 2;
            auto tWait = std::chrono::high_resolution_clock::now();
            if (timing) syncCompute(getCompute());   // exclude what was queued before (the previous frame)
            auto t0 = std::chrono::high_resolution_clock::now();
            m_waitSum += std::chrono::duration<float, std::milli>(t0 - tWait).count();

            m_solver->step();

            if (timing) {
                syncCompute(getCompute());
                m_elapsed = std::chrono::duration<float, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
                if (m_elapsed > 0.f) {
                    m_elapsedSum += m_elapsed;
                    m_fps.x += 1000.0f / m_elapsed;
                    m_fps.y += 1.f;
                    m_fps.z = m_fps.x / m_fps.y;
                }
            }
            if (m_options.checkStep != 0 && m_solver->getFrame() == m_options.checkStep) sanityCheck();
        }
    }

    // Graphics side: Particles::Draw and the grid, into the own target, then to the back buffer.
    void OnDrawOverlay(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer) override {
        if (m_rasterFramebuffer) {
            const nvrhi::FramebufferInfoEx &rasterInfo = m_rasterFramebuffer->getFramebufferInfo();
            RenderView view = getRenderView(int(rasterInfo.width), int(rasterInfo.height));

            commandList->clearTextureFloat(m_colorTexture, nvrhi::AllSubresources, nvrhi::Color(0.f, 0.f, 0.f, 1.f));
            commandList->clearDepthStencilTexture(m_depthTexture, nvrhi::AllSubresources, true, 1.f, true, 0);

            // Draw fluid
            std::vector<sph::SPHFluidInstance *> instances;
            sph::collectFluidInstances(getSceneGraph()->GetRootNode(), instances);
            m_pointRenderer->render(commandList, m_rasterFramebuffer, instances, view.viewProj);

            // Sketch a grid
            if (m_linesPipeline) {
                LinesConstants constants;
                constants.matViewProj = view.viewProj;
                commandList->writeBuffer(m_linesConstants, &constants, sizeof(constants));

                nvrhi::GraphicsState state;
                state.pipeline = m_linesPipeline;
                state.framebuffer = m_rasterFramebuffer;
                state.bindings = {m_linesSet};
                state.vertexBuffers = {nvrhi::VertexBufferBinding{m_linesVertices, 0, 0}};
                state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(rasterInfo.width), float(rasterInfo.height)));
                commandList->setGraphicsState(state);
                nvrhi::DrawArguments args;
                args.vertexCount = m_linesVertexCount;
                commandList->draw(args);
            }
        }

        if (m_presentSet) {
            const nvrhi::FramebufferInfoEx &fbInfo = framebuffer->getFramebufferInfo();
            nvrhi::GraphicsState state;
            state.pipeline = m_presentPipeline;
            state.framebuffer = framebuffer;
            state.bindings = {m_presentSet};
            state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(fbInfo.width), float(fbInfo.height)));
            commandList->setGraphicsState(state);
            dm::float4 screenST = {1.f, 1.f, 0.f, 0.f};
            commandList->setPushConstants(&screenST, sizeof(screenST));
            nvrhi::DrawArguments args;
            args.vertexCount = 4;
            commandList->draw(args);
        }
    }

    void OnBuildUI() override {
        m_inspectorFrame = getFrameCounter();
        ImGui::Text("Space = Pause sim");
        ImGui::Text("R key = Reset sim");
        ImGui::Text("# Particles:  %u", m_particles->getNumParticles());
        if (m_measureTiming && m_fps.y > 0.f)
            ImGui::Text("Compute: %4.0f FPS (%4.2f msec)", m_fps.z, 1000.0f / m_fps.z);
        else
            ImGui::Text("Compute: not measured");
        ImGui::Text("Time: %.3f s, step %u", m_solver->getTime(), m_solver->getFrame());
        ImGui::Checkbox("Measure compute time (waits for the GPU)", &m_measureTiming);
        if (ImGui::Button(m_paused ? "Resume" : "Pause")) m_paused = !m_paused;
        ImGui::SameLine();
        if (ImGui::Button("Reset")) m_resetRequested = true;
        ImGui::Text("Right or Alt+Left: orbit, Middle: pan, Wheel: zoom");
    }

    bool OnKey(int key, int scancode, int action, int mods) override {
        if (action != GLFW_PRESS) return false;
        switch (key) {
            case GLFW_KEY_SPACE: m_paused = !m_paused; return true;
            case GLFW_KEY_R: m_resetRequested = true; return true;
            default: return false;
        }
    }

    // Sample::motion of the reference.
    void OnMouseDrag(int button, int dx, int dy, int mods) override {
        const bool alt = (mods & GLFW_MOD_ALT) != 0;
        const float fine = 0.5f;
        OrbitController &cam = getCameraOrbit();
        switch (button) {
            case GLFW_MOUSE_BUTTON_LEFT:
                if (!alt) break;
                [[fallthrough]];
            case GLFW_MOUSE_BUTTON_RIGHT: {
                // Adjust camera orbit
                dm::float3 angles = cam.angles;
                angles.x += float(dx) * 0.2f * fine;
                angles.y -= float(dy) * 0.2f * fine;
                cam.setOrbit(angles, cam.target, cam.distance);
            } break;
            case GLFW_MOUSE_BUTTON_MIDDLE:
                // Adjust target pos
                cam.moveRelative(float(dx) * fine * cam.distance / 1000.f, float(-dy) * fine * cam.distance / 1000.f, 0.f);
                break;
            default: break;
        }
    }

    // Sample::mousewheel of the reference (its delta is in WHEEL_DELTA units, 120 per notch).
    bool MouseScrollUpdate(double xoffset, double yoffset) override {
        OrbitController &cam = getCameraOrbit();
        const float delta = float(yoffset) * 120.f;
        const float zoomamt = 1.0f;
        float dist = cam.distance;
        const float zoom = (dist - kReferenceDolly) * 0.001f;
        dist -= delta * zoom * zoomamt;
        cam.setOrbit(cam.angles, cam.target, dist);
        return true;
    }

 private:
    SPHFluidPoolOptions m_options;
    nvrhi::AutoPtr<sph::ISPHParticles> m_particles;
    nvrhi::AutoPtr<sph::ISPHSolver> m_solver;
    nvrhi::AutoPtr<sph::SPHFluidInstance> m_instance;
    std::unique_ptr<sph::SPHPointRenderer> m_pointRenderer;

    bool m_paused = false;
    bool m_resetRequested = false;
    bool m_measureTiming = true;
    uint32_t m_inspectorFrame = 0;
    float m_elapsed = 0.f;
    float m_elapsedSum = 0.f;
    // run statistics, logged at exit
    uint32_t m_frames = 0;
    std::chrono::high_resolution_clock::time_point m_firstFrameTime;
    float m_waitSum = 0.f;
    dm::float3 m_fps = dm::float3(0.f);   // m_fps of the reference: sum of FPS, steps, average

    // own colour + depth target and its presentation
    nvrhi::TextureHandle m_colorTexture;
    nvrhi::TextureHandle m_depthTexture;
    nvrhi::FramebufferHandle m_rasterFramebuffer;
    nvrhi::GraphicsPipelineHandle m_presentPipeline;
    nvrhi::BindingLayoutHandle m_presentLayout;
    nvrhi::BindingSetHandle m_presentSet;
    nvrhi::SamplerHandle m_sampler;

    // ground grid
    nvrhi::ShaderHandle m_linesVS, m_linesPS;
    nvrhi::InputLayoutHandle m_linesInputLayout;
    nvrhi::BufferHandle m_linesConstants;
    nvrhi::BufferHandle m_linesVertices;
    nvrhi::BindingLayoutHandle m_linesLayout;
    nvrhi::BindingSetHandle m_linesSet;
    nvrhi::GraphicsPipelineHandle m_linesPipeline;
    uint32_t m_linesVertexCount = 0;
};

int main(int argc, const char **argv) {
    SPHFluidPoolOptions sphOptions;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--particles") == 0 && i + 1 < argc) {
            long long n = atoll(argv[++i]);
            if (n < 1 || n > 100000000ll) {
                fprintf(stderr, "--particles: expected 1 .. 100000000\n");
                return 1;
            }
            sphOptions.maxParticles = uint32_t(n);
        } else if (strcmp(argv[i], "--example") == 0 && i + 1 < argc) {
            int n = atoi(argv[++i]);
            if (n < 0 || n > 3) {
                fprintf(stderr, "--example: expected 0 .. 3\n");
                return 1;
            }
            sphOptions.example = n;
        } else if (strcmp(argv[i], "--paused") == 0) {
            sphOptions.paused = true;
        } else if (strcmp(argv[i], "--check") == 0 && i + 1 < argc) {
            sphOptions.checkStep = uint32_t(std::max(atoi(argv[++i]), 0));
        }
    }

    GVDBAppOptions options;
    options.title = "Fluids v5.0 - sph_fluid_pool";
    options.sampleName = GVDB_SAMPLE_NAME;
    options.width = 1270;
    options.height = 800;
    options.vsync = false;
    return runGVDBApp(argc, argv, options, [&](app::DeviceManager *dm) -> nvrhi::AutoPtr<GVDBApp> {
        return nvrhi::TakeOver(MAKE_RC_OBJ(SPHFluidPoolApp, dm, sphOptions));
    });
}

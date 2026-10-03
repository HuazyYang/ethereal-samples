// fluid-surface (gFluidSurface): an SPH fluid of 1.5M particles simulated on
// the GPU (FluidSystem, kernels in FluidSystemKernels.cu) is surfaced through
// GVDB every frame: the particle positions (and colours) feed IGVDBPointOps
// (setPoints -> rebuildTopology -> insertPointsSubcell -> gatherLevelSet),
// the level set is rendered by the OptiX renderer as a custom primitive next
// to ground.obj, progressively sampled while the simulation is paused.
//
// Keys (reference mapping): Space = simulate on/off, 1 = topology, 2 = fluid
// particles (lines), 3 = cycle the shading (off / level set / trilinear /
// volume / empty skipping), 4 = colour channel on/off, R = reset, ` = GUI.
// Mouse: GVDBApp orbit controls (left: orbit, middle: pan, right: distance,
// shift: light). Command line: --shading off|levelset|trilinear|volume|emptyskip
// selects the initial shading; --screenshot <file.png> [--frames N] [--samples M]
// (GVDBApp) simulates N frames, accumulates M OptiX samples (default 8) of the
// last state, saves the back buffer and exits.
#include <sample-utils/GVDBApp.h>
#include <sample-utils/OptixRenderer.h>
#include <sample-utils/ObjMesh.h>
#include <sample-utils/SampleTypes.h>
#include <sample-utils/ImageIO.h>
#include <gvdb/GVDB.h>
#include <donut/core/log.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include "FluidSystem.h"

using namespace donut;
using namespace SampleUtils;

namespace {

// Shading styles of the reference's '3' key: 0 = off (no surface is built),
// then the OptiX-capable shadings. The reference always traced SHADE_LEVELSET.
const char *const c_ShadeNames[] = {"Off (no surface)", "Level set", "Trilinear surface", "Volume (deep)",
                                    "Empty skipping"};
const gvdb::VolumeShading c_ShadeValues[] = {gvdb::VolumeShading::LevelSet, gvdb::VolumeShading::LevelSet,
                                             gvdb::VolumeShading::Trilinear, gvdb::VolumeShading::Volume,
                                             gvdb::VolumeShading::EmptySkip};
constexpr int c_NumShadeStyles = 5;

float elapsedMs(std::chrono::high_resolution_clock::time_point t0) {
    return std::chrono::duration<float, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
}

}  // namespace

class FluidSurfaceApp : public GVDBApp {
    NVRHI_INHERIT_INTERFACE_TABLE()
 public:
    using GVDBApp::GVDBApp;

    // --samples: OptiX samples accumulated before the GVDBApp screenshot.
    void setScreenshotSamples(int samples) { m_screenshotSamples = std::max(1, samples); }
    // --shading: initial shading style (index into c_ShadeNames).
    void setInitialShadeStyle(int style) { m_shadeStyle = dm::clamp(style, 0, c_NumShadeStyles - 1); }

    bool OnInit() override {
        if (!getRTDevice()) {
            log::error("fluid-surface: no OptiX device (OptiX SDK not found at build time or no driver support).");
            return false;
        }
        // Unattended run: GVDBApp saves the back buffer after m_screenshotFrames
        // presented frames. One simulation step per frame for the requested
        // frames, then (samples - 1) frames that only accumulate samples.
        if (isScreenshotRun()) {
            m_simFrames = int(m_screenshotFrames);
            m_screenshotFrames += uint32_t(m_screenshotSamples - 1);
        }
        srand(6572);

        // ---- fluid system ----
        log::info("Starting Fluid System.");
        m_fluid = std::make_unique<FluidSystem>(getGPDevice(), getGPQueue());
        if (!m_fluid->initialize(getVFS())) return false;
        if (!m_fluid->start(m_numPnts)) return false;

        // ---- volume (reconfigure of the reference) ----
        m_volume = createVolume();
        if (!m_volume) return false;
        m_volume->configure(gvdb::GVDBLevelConfig::fromBranching(3, 3, 3, 3, 5));
        m_volume->configureAtlas(gvdb::GVDBAtlasConfig());   // SetChannelDefault(16, 16, 1)
        if (NVRHI_FAILED(gvdb::createGVDBVoxelOps(m_volume, &m_ops))) return false;
        if (NVRHI_FAILED(gvdb::createGVDBPointOps(m_volume, &m_points))) return false;

        // Volume params of the reference: SetSteps, SetExtinct, SetVolumeRange(0, 3, -1),
        // SetCutoff; the index space of the fluid is the world (identity transform).
        m_instance = addVolumeInstance(m_volume, dm::affine3::identity(), "fluid");
        gvdb::VolumeRenderAttributes &attrs = m_instance->GetRenderAttributes();
        attrs.channel = 0;
        attrs.shading = c_ShadeValues[m_shadeStyle];
        attrs.steps = {0.25f, 16.f, 0.25f};
        attrs.extinct = {-1.0f, 1.5f, 0.0f};
        attrs.threshold = {0.0f, 3.0f, -1.0f};
        attrs.cutoff = {0.005f, 0.01f, 0.0f};
        attrs.transferFunction = MAKE_RC_OBJ_PTR(gvdb::VolumeTransferFunction, getGPDevice());
        attrs.transferFunction->setLinear(0.f, 1.f, {0.f, 0.f, 0.f, 0.f}, {1.f, 1.f, 1.f, 0.1f});   // library default
        attrs.transferFunction->commit(getGPQueue());
        reconfigureChannels();

        // ---- OptiX renderer, materials, environment (RebuildOptixGraph) ----
        m_renderer = std::make_unique<OptixRenderer>(getGPDevice(), getRTDevice(), getGPQueue(), getVFS());
        m_renderer->getViewParams().backgroundColor = {0.8f, 0.8f, 0.8f, 1.f};
        {
            std::vector<uint8_t> rgba;
            uint32_t w = 0, h = 0;
            if (loadPNG(getAssetPath("sky.png"), rgba, w, h)) m_renderer->setEnvmap(rgba.data(), w, h);
            else log::warning("fluid-surface: cannot load sky.png, white environment");
        }
        OptixMaterialParams surf;
        surf.lightWidth = 1.2f;
        surf.shadowWidth = 0.1f;
        surf.shadowBias = 0.5f;
        surf.ambColor = {.05f, .05f, .05f};
        surf.diffColor = {.7f, .7f, .7f};
        surf.specColor = {1.f, 1.f, 1.f};
        surf.specPower = 400.f;
        surf.envColor = {0.f, 0.f, 0.f};
        surf.reflWidth = 0.5f;
        surf.reflBias = 0.5f;
        surf.reflColor = {0.4f, 0.4f, 0.4f};
        surf.refrWidth = 0.0f;
        surf.refrColor = {0.1f, .1f, .1f};
        surf.refrIor = 1.1f;
        surf.refrAmount = 0.5f;
        surf.refrOffset = 50.0f;
        surf.refrBias = 0.5f;
        m_matSurf = m_renderer->addMaterial(surf);
        attrs.materialId = m_matSurf;
        m_renderer->setMeshMaterial(m_matSurf);

        // ---- ground polygons: ground.obj, pre-translated by (0, 10, 0), rotated 5 deg about Z ----
        m_mesh = loadObjMesh(nullptr, getAssetPath("ground.obj"), 1.f);
        if (m_mesh) {
            m_meshInstance = MAKE_RC_OBJ_PTR(engine::MeshInstance, m_mesh);
            engine::SceneGraphNode *node = getSceneGraph()->AttachLeafNode(getSceneGraph()->GetRootNode(), m_meshInstance);
            node->SetName("ground");
            setNodeTransform(node, dm::translation(dm::float3(0.f, 10.f, 0.f)) *
                                       dm::rotation(dm::float3(0.f, 0.f, 1.f), dm::radians(5.f)));
        } else {
            log::warning("fluid-surface: cannot load ground.obj");
        }

        // ---- camera and light (orbit the centre of the fluid grid) ----
        dm::float3 ctr = (m_fluid->getGridMax() + m_fluid->getGridMin()) * 0.5f;
        getCamera()->zNear = 1.f;
        getCamera()->zFar = 20000.f;
        getCameraOrbit().setOrbit({50.f, 30.f, 0.f}, ctr, 1200.f);
        getLightOrbit().setOrbit({20.f, 60.f, 0.f}, ctr, 1000.f);
        updateScene();

        // Surface of the initial particle block, so that the first frame shows the
        // fluid. The volume has bounds now: updateVolumes() builds the OptiX scene
        // (a GAS built from an empty volume's bounds would only be refitted later).
        log::info("Building the OptiX scene.");
        buildSurface();
        {
            const dm::box3 b = m_instance->GetLocalBoundingBox();
            const dm::float3 eye = getCameraOrbit().getPosition();
            log::info("fluid-surface: volume bounds (%.0f %.0f %.0f)-(%.0f %.0f %.0f), %llu bricks, camera (%.0f %.0f %.0f)",
                      b.m_mins.x, b.m_mins.y, b.m_mins.z, b.m_maxs.x, b.m_maxs.y, b.m_maxs.z,
                      (unsigned long long)m_volume->getNumActiveBricks(), eye.x, eye.y, eye.z);
        }
        log::info("Running..");
        return true;
    }

    // The channels of the reference's reconfigure(): level set + optional colour.
    void reconfigureChannels() {
        m_volume->destroyChannels();
        gvdb::GVDBChannelDesc levelSet;
        levelSet.format = gvdb::ATLAS_FORMAT_R32_FLOAT;
        m_volume->addChannel(levelSet);
        if (m_useColor) {
            gvdb::GVDBChannelDesc color;
            color.format = gvdb::ATLAS_FORMAT_RGBA8_UINT;
            m_volume->addChannel(color);
        }
        m_instance->GetRenderAttributes().colorChannel = m_useColor ? 1 : gvdb::CHAN_UNDEF;
        m_points->requestFullRebuild(true);
    }

    // Points -> topology -> level set (the GVDB part of the reference's simulate()).
    void buildSurface() {
        const uint32_t n = uint32_t(m_fluid->getNumPoints());
        if (n == 0) return;
        auto t0 = std::chrono::high_resolution_clock::now();

        gvdb::PointData pos, vel, clr;
        pos.buffer = m_fluid->getBuffer(FPOS);
        pos.offset = 0;
        pos.stride = sizeof(float) * 3;
        pos.count = n;
        if (m_useColor) {
            clr.buffer = m_fluid->getBuffer(FCLR);
            clr.offset = 0;
            clr.stride = sizeof(uint32_t);
            clr.count = n;
        }
        m_points->setPoints(pos, vel, clr);

        // topology (GPU rebuild), atlas
        m_points->rebuildTopology(n, m_radius * 2.0f, m_origin);
        m_volume->finishTopology(true);
        m_volume->updateAtlas();

        // insert and gather points -> voxels
        int scPntLen = 0;
        const int subcellSize = 4;
        m_points->insertPointsSubcell(subcellSize, n, m_radius * 2.0f, m_origin, scPntLen);
        m_points->gatherLevelSet(subcellSize, n, m_radius, m_origin, scPntLen, 0, 1);
        m_ops->updateApron(0, 3.0f);
        if (m_useColor) m_ops->updateApron(1, 0.0f);

        // OptiX: the topology changed
        m_renderer->updateVolumes(getSceneGraph());
        syncQueue(getGPDevice(), getGPQueue());
        m_msSurface = elapsedMs(t0);
    }

    void simulate() {
        auto t0 = std::chrono::high_resolution_clock::now();
        m_fluid->run();
        syncQueue(getGPDevice(), getGPQueue());
        m_msFluid = elapsedMs(t0);
        if (m_shadeStyle == 0) return;   // no surface while the shading is off
        buildSurface();
    }

    void reset() {
        if (!m_fluid->start(m_numPnts)) return;
        m_points->requestFullRebuild(true);
        m_frame = 0;
        m_sample = 0;
        buildSurface();
    }

    void applyShading() {
        m_instance->GetRenderAttributes().shading = c_ShadeValues[m_shadeStyle];
        m_sample = 0;
    }

    void OnRender(uint32_t width, uint32_t height) override {
        if (m_renderer->getWidth() != width || m_renderer->getHeight() != height) {
            m_renderer->resizeOutput(width, height);
            m_sample = 0;
        }
        getCamera()->verticalFov = verticalFovFromHorizontal(dm::radians(50.f), float(width) / float(height));

        const bool stopForScreenshot = isScreenshotRun() && m_frame >= m_simFrames;
        if (m_simulate && !stopForScreenshot) {
            simulate();
            m_frame++;
            m_sample = 0;
            if (isScreenshotRun() && m_frame == m_simFrames)
                log::info("fluid-surface: %d frames simulated; last frame: fluid %.2f ms, surface %.2f ms, %llu bricks",
                          m_frame, m_msFluid, m_msSurface, (unsigned long long)m_volume->getNumActiveBricks());
        }

        m_renderer->setSample(m_frame, m_sample++);
        m_renderer->render(getSceneGraph(), getRenderView(int(width), int(height)));
        UT_V_GP(getGPQueue()->copyBufferRegion(getPresenter()->getRenderBuffer(0), 0, m_renderer->getOutputBuffer(), 0,
                                               size_t(width) * height * 4));

        if (m_showGui) {
            if (m_showFluid) drawFluid();
            if (m_showTopo) getDebugDraw()->topology(m_instance);
        }
    }

    // The reference's draw_fluid: a line per particle from its position along its
    // velocity, in the particle colour. Every 8th particle (1.5M lines are too
    // many for the overlay).
    void drawFluid() {
        m_fluid->readback(m_hostPos, m_hostVel, m_hostClr);
        const dm::affine3 toWorld = m_instance->GetIndexToWorld();
        for (size_t n = 0; n < m_hostPos.size(); n += 8) {
            dm::float3 p1 = m_hostPos[n] + m_origin;
            dm::float3 p2 = p1 + m_hostVel[n] + dm::float3(.1f, .1f, .1f);
            uint32_t c = m_hostClr[n];
            dm::float4 color(float(c & 255) / 255.f, float((c >> 8) & 255) / 255.f, float((c >> 16) & 255) / 255.f, 1.f);
            getDebugDraw()->line(toWorld.transformPoint(p1), toWorld.transformPoint(p2), color);
        }
    }

    void OnMouseDrag(int button, int dx, int dy, int mods) override {
        GVDBApp::OnMouseDrag(button, dx, dy, mods);
        m_sample = 0;   // camera or light moved
    }

    void OnBuildUI() override {
        if (!m_showGui) return;
        ImGui::Checkbox("Simulate", &m_simulate);
        ImGui::Checkbox("Topology", &m_showTopo);
        ImGui::Checkbox("Fluid", &m_showFluid);
        if (ImGui::Checkbox("Color", &m_useColor)) {
            reconfigureChannels();
            buildSurface();
            m_sample = 0;
        }
        if (ImGui::Combo("Shading", &m_shadeStyle, c_ShadeNames, c_NumShadeStyles)) applyShading();
        if (ImGui::Button("Reset")) reset();
        ImGui::Separator();
        ImGui::Text("%d particles, t = %.3f s", m_fluid->getNumPoints(), m_fluid->getTime());
        ImGui::Text("Bricks: %llu", (unsigned long long)m_volume->getNumActiveBricks());
        ImGui::Text("Fluid %.1f ms, surface %.1f ms", m_msFluid, m_msSurface);
        ImGui::Text("Frame %d, sample %d", m_frame, m_sample);
        ImGui::Text("Space: simulate, 1: topology, 2: fluid, 3: shading, 4: color, R: reset");
        ImGui::Text("Left: orbit, Middle: pan, Right: distance, Shift: light");
    }

    bool OnKey(int key, int scancode, int action, int mods) override {
        if (action != GLFW_PRESS) return false;
        switch (key) {
            case GLFW_KEY_GRAVE_ACCENT: m_showGui = !m_showGui; return true;
            case GLFW_KEY_1: m_showTopo = !m_showTopo; return true;
            case GLFW_KEY_2: m_showFluid = !m_showFluid; return true;
            case GLFW_KEY_3:
                m_shadeStyle = (m_shadeStyle + 1) % c_NumShadeStyles;
                applyShading();
                return true;
            case GLFW_KEY_4:
                m_useColor = !m_useColor;
                reconfigureChannels();
                buildSurface();
                m_sample = 0;
                return true;
            case GLFW_KEY_SPACE: m_simulate = !m_simulate; return true;
            case GLFW_KEY_R: reset(); return true;
            default: return false;
        }
    }

 private:
    std::unique_ptr<FluidSystem> m_fluid;
    nvrhi::AutoPtr<gvdb::IGVDBVolume> m_volume;
    nvrhi::AutoPtr<gvdb::IGVDBVoxelOps> m_ops;
    nvrhi::AutoPtr<gvdb::IGVDBPointOps> m_points;
    nvrhi::AutoPtr<gvdb::GVDBVolumeInstance> m_instance;
    nvrhi::AutoPtr<engine::MeshInfo> m_mesh;
    nvrhi::AutoPtr<engine::MeshInstance> m_meshInstance;
    std::unique_ptr<OptixRenderer> m_renderer;
    std::vector<dm::float3> m_hostPos, m_hostVel;
    std::vector<uint32_t> m_hostClr;

    int m_numPnts = 1500000;
    dm::float3 m_origin = dm::float3::zero();
    float m_radius = 1.0f;
    bool m_simulate = true;
    bool m_showGui = true;
    bool m_showFluid = false;
    bool m_showTopo = false;
    bool m_useColor = true;
    int m_shadeStyle = 1;   // level set
    int m_matSurf = 0;
    int m_frame = 0;
    int m_sample = 0;
    float m_msFluid = 0.f;
    float m_msSurface = 0.f;
    int m_simFrames = 0;
    int m_screenshotSamples = 8;
};

int main(int argc, const char **argv) {
    int samples = 8;
    int shadeStyle = 1;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
            samples = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--shading") == 0 && i + 1 < argc) {
            const char *name = argv[++i];
            if (strcmp(name, "off") == 0) shadeStyle = 0;
            else if (strcmp(name, "trilinear") == 0) shadeStyle = 2;
            else if (strcmp(name, "volume") == 0) shadeStyle = 3;
            else if (strcmp(name, "emptyskip") == 0) shadeStyle = 4;
            else shadeStyle = 1;   // levelset
        }
    }

    GVDBAppOptions options;
    options.title = "GVDB Voxels - fluid-surface";
    options.sampleName = GVDB_SAMPLE_NAME;
    options.wantOptix = true;
    return runGVDBApp(argc, argv, options, [&](app::DeviceManager *dm) -> nvrhi::AutoPtr<GVDBApp> {
        auto app = MAKE_RC_OBJ_PTR(FluidSurfaceApp, dm);
        app->setScreenshotSamples(samples);
        app->setInitialShadeStyle(shadeStyle);
        return app;
    });
}

// SPH wave pool on NVRHI graphics compute or CUDA, with offline Slang shaders.
// SPDX-License-Identifier: Apache-2.0
#include "PortablePool.h"
#include "ExampleDevice.h"
#include <donut/engine/ShaderFactory.h>
#include <donut/app/ImGuiRenderPass.h>
#include <imgui.h>
#include <donut/core/vfs/VFS.h>
#include <nvrhi/utils.h>
#include <GLFW/glfw3.h>
#include <stb_image_write.h>
#include <chrono>
#include <fstream>
#include <memory>
#include <algorithm>
using namespace donut;
namespace g = gvdb::slang;
namespace es = ethereal::slang;
namespace {
void check(nvrhi::FRESULT rc, const char* action) {
    if (NVRHI_FAILED(rc))
        throw std::runtime_error(std::string(action) + " failed: " + std::to_string(rc));
}
struct Options {
    uint32_t capacity = 4000000, steps = 120, checkStep = 0;
    int example = 2, adapter = -1;
    bool headless = false, paused = false, validation = false, cuda = false;
    bool bounded = false, showUI = true, guiInputTest = false;
    std::string screenshot, csv;
};
g::Float4 vector4(dm::float3 v, float w = 0) { return {v.x, v.y, v.z, w}; }
struct ViewParameters {
    g::Float4 eye, right, up, forward, projection, extent;
};
void report(g::SlangSphSimulation& simulation) {
    const auto points = simulation.readPositions(), velocities = simulation.readVelocities();
    dm::float3 lower(INFINITY), upper(-INFINITY);
    double speed = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        const auto& p = points[i];
        const auto& v = velocities[i];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(v.x) ||
            !std::isfinite(v.y) || !std::isfinite(v.z))
            throw std::runtime_error("SPH generated a nonfinite particle");
        if (p.x == -1000 && p.y == -1000 && p.z == -1000)
            throw std::runtime_error("SPH particle left the acceleration grid");
        lower = dm::min(lower, dm::float3(p.x, p.y, p.z));
        upper = dm::max(upper, dm::float3(p.x, p.y, p.z));
        speed = std::max(speed, std::sqrt(double(v.x) * v.x + double(v.y) * v.y + double(v.z) * v.z));
    }
    std::printf("%zu particles; time %.6f; bounds [%g,%g,%g]..[%g,%g,%g]; maximum speed %.6g\n", points.size(),
                simulation.parameters().integration.x, lower.x, lower.y, lower.z, upper.x, upper.y, upper.z,
                speed);
}
void writeCsv(g::SlangSphSimulation& simulation, const std::string& path) {
    if (path.empty())
        return;
    const auto points = simulation.readPositions(), velocities = simulation.readVelocities();
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("Cannot create particle CSV");
    out.precision(9);
    out << "x,y,z,vx,vy,vz\n";
    for (size_t i = 0; i < points.size(); ++i)
        out << points[i].x << ',' << points[i].y << ',' << points[i].z << ',' << velocities[i].x << ','
            << velocities[i].y << ',' << velocities[i].z << '\n';
    if (!out)
        throw std::runtime_error("Cannot write particle CSV");
}
class PoolView : public app::IRenderPass {
    NVRHI_INHERIT_INTERFACE_TABLE()
    Options m_options;
    sph::PortablePool m_initial;
    es::ComputeRuntime m_runtime;
    std::unique_ptr<g::SlangSphSimulation> m_simulation;
    nvrhi::ShaderHandle m_vs, m_ps;
    nvrhi::BindingLayoutHandle m_layout;
    nvrhi::GraphicsPipelineHandle m_pipeline;
    nvrhi::BufferHandle m_constants;
    nvrhi::CommandListHandle m_commands;
    dm::float3 m_center;
    float m_distance, m_yaw = 0.7f, m_pitch = 0.55f;
    uint32_t m_frame = 0, m_rendered = 0;
    bool m_paused;
    bool m_resetRequested = false, m_showUI = true, m_measureTiming = false;
    double m_lastComputeMs = 0, m_computeMsSum = 0;
    uint32_t m_timedSteps = 0;
    double m_mouseX = 0, m_mouseY = 0;
    bool m_mouseKnown = false;
    int m_drag = -1;

 public:
    PoolView(app::DeviceManager* manager, const Options& options)
        : IRenderPass(manager),
          m_options(options),
          m_initial(options.capacity, sph::SPHExample(options.example)),
          m_runtime(GetDevice(), app::GetDirectoryWithExecutable() / "shaders/gvdb_slang" /
                                     app::GetShaderTypeName(GetDevice()->getGraphicsAPI())),
          m_paused(options.paused),
          m_showUI(options.showUI) {
        reset();
        dm::float3 lower(INFINITY), upper(-INFINITY);
        for (const auto& p : m_initial.positions) {
            lower = dm::min(lower, dm::float3(p.x, p.y, p.z));
            upper = dm::max(upper, dm::float3(p.x, p.y, p.z));
        }
        m_center = (lower + upper) * .5f;
        m_distance = std::max(10.f, dm::length(upper - lower) * 1.6f);
        auto fs = MAKE_RC_OBJ_PTR(vfs::NativeFileSystem);
        engine::ShaderFactory factory(GetDevice(), fs,
                                      app::GetDirectoryWithExecutable() / "shaders/sph_fluid_pool" /
                                          app::GetShaderTypeName(GetDevice()->getGraphicsAPI()));
        m_vs = factory.CreateShader("Points.slang", "main_vs", nullptr, nvrhi::ShaderType::Vertex);
        m_ps = factory.CreateShader("Points.slang", "main_ps", nullptr, nvrhi::ShaderType::Pixel);
        if (!m_vs || !m_ps)
            throw std::runtime_error("Cannot load particle graphics artifacts");
        nvrhi::BindingLayoutDesc layout;
        layout.visibility = nvrhi::ShaderType::Vertex;
        layout.bindings = {nvrhi::BindingLayoutItem::ConstantBuffer(0),
                           nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
                           nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1)};
        check(GetDevice()->createBindingLayout(layout, &m_layout), "create particle binding layout");
        nvrhi::BufferDesc desc;
        desc.byteSize = 256;
        desc.isConstantBuffer = true;
        desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
        desc.keepInitialState = true;
        check(GetDevice()->createBuffer(desc, &m_constants), "create view constants");
        check(GetDevice()->createCommandList(nvrhi::CommandListParameters(), &m_commands),
              "create particle commands");
    }
    void reset() {
        m_runtime.flush();
        auto replacement =
            std::make_unique<g::SlangSphSimulation>(m_runtime, m_initial.parameters, m_initial.positions);
        m_simulation = std::move(replacement);
        m_frame = 0;
        m_lastComputeMs = m_computeMsSum = 0;
        m_timedSteps = 0;
    }
    void flush() { m_runtime.flush(); }
    bool uiVisible() const { return m_showUI; }
    bool paused() const { return m_paused; }
    uint32_t frame() const { return m_frame; }
    dm::float3 cameraState() const { return {m_yaw, m_pitch, m_distance}; }
    dm::float3 cameraTarget() const { return m_center; }
    void cancelDrag() { m_drag = -1; }
    void capturedPointer(double x, double y) {
        m_mouseX = x;
        m_mouseY = y;
        m_mouseKnown = true;
        cancelDrag();
    }
    void buildUI() {
        ImGui::Text("Space = Pause sim; R = Reset sim");
        ImGui::Text("# Particles: %u", uint32_t(m_initial.positions.size()));
        ImGui::Text("Time: %.3f s, step %u", m_simulation->parameters().integration.x, m_frame);
        if (m_timedSteps)
            ImGui::Text("Compute: %.2f ms (last %.2f ms)", m_computeMsSum / m_timedSteps, m_lastComputeMs);
        else
            ImGui::TextUnformatted("Compute: not measured");
        ImGui::Checkbox("Measure compute time (waits for the GPU)", &m_measureTiming);
        if (ImGui::Button(m_paused ? "Resume" : "Pause"))
            m_paused = !m_paused;
        ImGui::SameLine();
        if (ImGui::Button("Reset"))
            m_resetRequested = true;
        ImGui::TextUnformatted("Right or Alt+Left: orbit; Middle: pan; Wheel: zoom");
        ImGui::TextUnformatted("` hides or shows the Inspector");
    }
    g::SlangSphSimulation& simulation() { return *m_simulation; }
    void step(uint32_t count) {
        m_simulation->step(count);
        m_runtime.flush();
        m_frame += count;
    }
    void BackBufferResizing() override { m_pipeline = nullptr; }
    bool KeyboardUpdate(int key, int, int action, int) override {
        if (action != GLFW_PRESS && action != GLFW_REPEAT)
            return false;
        if (action == GLFW_REPEAT &&
            (key == GLFW_KEY_SPACE || key == GLFW_KEY_R || key == GLFW_KEY_GRAVE_ACCENT))
            return true;
        if (key == GLFW_KEY_SPACE)
            m_paused = !m_paused;
        else if (key == GLFW_KEY_R)
            m_resetRequested = true;
        else if (key == GLFW_KEY_GRAVE_ACCENT)
            m_showUI = !m_showUI;
        else if (key == GLFW_KEY_RIGHT)
            m_yaw += .08f;
        else if (key == GLFW_KEY_LEFT)
            m_yaw -= .08f;
        else if (key == GLFW_KEY_UP)
            m_pitch = std::min(1.4f, m_pitch + .08f);
        else if (key == GLFW_KEY_DOWN)
            m_pitch = std::max(-1.4f, m_pitch - .08f);
        else
            return false;
        return true;
    }
    bool MouseScrollUpdate(double, double y) override {
        m_distance = std::max(1.f, m_distance * float(std::exp(-y * .1)));
        return true;
    }
    bool MouseButtonUpdate(int button, int action, int mods) override {
        if (action == GLFW_RELEASE) {
            cancelDrag();
            return false;
        }
        if (action != GLFW_PRESS)
            return false;
        if (button == GLFW_MOUSE_BUTTON_RIGHT || button == GLFW_MOUSE_BUTTON_MIDDLE ||
            (button == GLFW_MOUSE_BUTTON_LEFT && (mods & GLFW_MOD_ALT))) {
            m_drag = button;
            return true;
        }
        return false;
    }
    bool MousePosUpdate(double x, double y) override {
        const float dx = m_mouseKnown ? float(x - m_mouseX) : 0, dy = m_mouseKnown ? float(y - m_mouseY) : 0;
        m_mouseX = x;
        m_mouseY = y;
        m_mouseKnown = true;
        if (m_drag < 0)
            return false;
        if (m_drag == GLFW_MOUSE_BUTTON_MIDDLE) {
            const dm::float3 backwards(std::sin(m_yaw) * std::cos(m_pitch), std::sin(m_pitch),
                                       std::cos(m_yaw) * std::cos(m_pitch));
            const auto right = dm::normalize(dm::cross(-backwards, dm::float3(0, 1, 0))),
                       up = dm::cross(right, -backwards);
            m_center += right * (dx * .5f * m_distance / 1000.f) + up * (-dy * .5f * m_distance / 1000.f);
        } else {
            m_yaw += dm::radians(dx * .1f);
            m_pitch = std::clamp(m_pitch - dm::radians(dy * .1f), -1.4f, 1.4f);
        }
        return true;
    }
    void Animate(float) override {
        if (m_resetRequested) {
            reset();
            m_resetRequested = false;
        }
        if (!m_paused) {
            const bool timing = m_showUI && m_measureTiming;
            if (timing) {
                m_runtime.flush();
                if (!GetDevice()->waitForIdle())
                    throw std::runtime_error("Cannot start SPH timing");
            }
            const auto begin = std::chrono::steady_clock::now();
            step(1);
            if (timing) {
                if (!GetDevice()->waitForIdle())
                    throw std::runtime_error("SPH timed execution failed");
                m_lastComputeMs =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
                m_computeMsSum += m_lastComputeMs;
                ++m_timedSteps;
            }
            if (m_options.checkStep && m_frame == m_options.checkStep)
                report(*m_simulation);
        }
        GetDeviceManager()->SetInformativeWindowTitle(m_paused ? "Slang SPH - paused"
                                                               : "Slang SPH - wave pool");
    }
    void Render(nvrhi::IFramebuffer* framebuffer) override {
        m_runtime.flush();
        const auto viewport = framebuffer->getFramebufferInfo().getViewport();
        if (!m_pipeline) {
            nvrhi::GraphicsPipelineDesc p;
            p.VS = m_vs;
            p.PS = m_ps;
            p.bindingLayouts = {m_layout};
            p.primType = nvrhi::PrimitiveType::TriangleList;
            p.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
            p.renderState.depthStencilState.depthTestEnable = false;
            p.renderState.depthStencilState.depthWriteEnable = false;
            check(GetDevice()->createGraphicsPipeline1(p, framebuffer->getFramebufferInfo().getInfo(),
                                                       &m_pipeline),
                  "create particle pipeline");
        }
        const dm::float3 backwards(std::sin(m_yaw) * std::cos(m_pitch), std::sin(m_pitch),
                                   std::cos(m_yaw) * std::cos(m_pitch));
        const auto forward = -backwards, right = dm::normalize(dm::cross(forward, dm::float3(0, 1, 0))),
                   up = dm::cross(right, forward);
        ViewParameters view{
            vector4(m_center + backwards * m_distance),
            vector4(right),
            vector4(up),
            vector4(forward),
            {0.65f,            0.65f * viewport.height() / viewport.width(), 1.5f, 0.1f},
            {viewport.width(), viewport.height(),                            .05f, 0   }
        };
        uint8_t padded[256]{};
        std::memcpy(padded, &view, sizeof(view));
        nvrhi::BindingSetDesc bindings;
        bindings.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_constants),
                             nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_simulation->positions()),
                             nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_simulation->velocities())};
        nvrhi::BindingSetHandle set;
        check(GetDevice()->createBindingSet(bindings, m_layout, &set), "bind simulation to rasterizer");
        m_commands->open();
        m_commands->writeBuffer(m_constants, padded, sizeof(padded));
        nvrhi::utils::ClearColorAttachment(m_commands, framebuffer, 0, nvrhi::Color(.025f, .04f, .07f, 1));
        nvrhi::GraphicsState state;
        state.pipeline = m_pipeline;
        state.framebuffer = framebuffer;
        state.bindings = {set};
        state.viewport.addViewportAndScissorRect(viewport);
        m_commands->setGraphicsState(state);
        nvrhi::DrawArguments args;
        args.vertexCount = 6;
        args.instanceCount = uint32_t(m_initial.positions.size());
        m_commands->draw(args);
        m_commands->close();
        GetDevice()->executeCommandList(m_commands);
        if (!m_options.headless && m_options.bounded && ++m_rendered >= m_options.steps)
            glfwSetWindowShouldClose(GetDeviceManager()->GetWindow(), GLFW_TRUE);
    }
};
class PoolGuiPass : public app::ImGuiRenderPass {
    NVRHI_INHERIT_INTERFACE_TABLE()
    PoolView& m_view;

 public:
    PoolGuiPass(app::DeviceManager* manager, PoolView& view)
        : ImGuiRenderPass(manager), m_view(view) {}
    bool MousePosUpdate(double x, double y) override {
        const bool captured = ImGuiRenderPass::MousePosUpdate(x, y) && m_view.uiVisible();
        if (captured)
            m_view.capturedPointer(x, y);
        return captured;
    }
    bool MouseButtonUpdate(int button, int action, int mods) override {
        const bool captured = ImGuiRenderPass::MouseButtonUpdate(button, action, mods) && m_view.uiVisible();
        if (captured || action == GLFW_RELEASE)
            m_view.cancelDrag();
        return captured;
    }
    bool MouseScrollUpdate(double x, double y) override {
        return ImGuiRenderPass::MouseScrollUpdate(x, y) && m_view.uiVisible();
    }
    bool KeyboardUpdate(int key, int scan, int action, int mods) override {
        const bool captured = ImGuiRenderPass::KeyboardUpdate(key, scan, action, mods) && m_view.uiVisible();
        return key != GLFW_KEY_GRAVE_ACCENT && captured;
    }
    bool KeyboardCharInput(unsigned int c, int mods) override {
        return ImGuiRenderPass::KeyboardCharInput(c, mods) && m_view.uiVisible();
    }

 protected:
    void BuildUI() override {
        if (!m_view.uiVisible())
            return;
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(370, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Inspector"))
            m_view.buildUI();
        ImGui::End();
    }
};
struct ShutdownDevice {
    app::DeviceManager* manager;
    ~ShutdownDevice() { manager->Shutdown(); }
};
struct RegisteredPasses {
    app::DeviceManager* manager;
    app::IRenderPass* view;
    app::IRenderPass* gui;
    ~RegisteredPasses() {
        if (gui)
            manager->RemoveRenderPass(gui);
        if (view)
            manager->RemoveRenderPass(view);
        manager->GetDevice()->waitForIdle();
    }
};
// Inspect actual GUI geometry/input on an owned target, before swapchain acquisition.
void exerciseGui(PoolView& view, PoolGuiPass& gui, app::DeviceManager& manager) {
    const auto require = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    view.flush();
    nvrhi::TextureDesc desc;
    desc.width = 640;
    desc.height = 480;
    desc.format = manager.GetFramebuffer(0)->getFramebufferInfo().colorFormats[0];
    desc.isRenderTarget = true;
    desc.initialState = nvrhi::ResourceStates::RenderTarget;
    desc.keepInitialState = true;
    nvrhi::TextureHandle target;
    check(manager.GetDevice()->createTexture(desc, &target), "create Inspector test target");
    nvrhi::FramebufferHandle fb;
    check(manager.GetDevice()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target), &fb),
          "create Inspector test framebuffer");
    const auto draw = [&]() {
        gui.Animate(0);
        gui.Render(fb);
        return ImGui::GetDrawData()->TotalVtxCount;
    };
    nvrhi::CommandListHandle clear;
    check(manager.GetDevice()->createCommandList(nvrhi::CommandListParameters(), &clear),
          "create Inspector clear");
    clear->open();
    clear->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(0.f));
    clear->close();
    manager.GetDevice()->executeCommandList(clear);
    (void)draw();
    require(draw() > 0, "SPH Inspector has no visible geometry");
    auto& io = ImGui::GetIO();
    io.WantCaptureMouse = io.WantCaptureKeyboard = true;
    require(gui.KeyboardUpdate(GLFW_KEY_SPACE, 0, GLFW_PRESS, 0) && gui.MouseScrollUpdate(0, 1),
            "Visible SPH Inspector did not capture input");
    gui.KeyboardUpdate(GLFW_KEY_SPACE, 0, GLFW_RELEASE, 0);
    require(!gui.KeyboardUpdate(GLFW_KEY_GRAVE_ACCENT, 0, GLFW_PRESS, 0),
            "Inspector visibility key is swallowed");
    view.KeyboardUpdate(GLFW_KEY_GRAVE_ACCENT, 0, GLFW_PRESS, 0);
    require(!view.uiVisible(), "Inspector hide failed");
    io.WantCaptureMouse = io.WantCaptureKeyboard = true;
    const bool paused = view.paused();
    view.KeyboardUpdate(GLFW_KEY_SPACE, 0, GLFW_REPEAT, 0);
    require(view.paused() == paused, "Key repeat repeatedly toggled pause");
    require(!gui.KeyboardUpdate(GLFW_KEY_SPACE, 0, GLFW_PRESS, 0), "Hidden SPH Inspector captured key");
    view.KeyboardUpdate(GLFW_KEY_SPACE, 0, GLFW_PRESS, 0);
    require(view.paused() != paused, "Pause was not forwarded once");
    view.KeyboardUpdate(GLFW_KEY_SPACE, 0, GLFW_PRESS, 0);
    require(!gui.MousePosUpdate(100, 100), "Hidden SPH Inspector captured pointer");
    view.MousePosUpdate(100, 100);
    require(!gui.MouseButtonUpdate(GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS, 0),
            "Hidden SPH Inspector captured button");
    view.MouseButtonUpdate(GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS, 0);
    const auto camera = view.cameraState();
    view.MousePosUpdate(110, 95);
    const auto orbit = view.cameraState();
    require(std::abs(orbit.x - camera.x - dm::radians(1.f)) < 1e-6f &&
                std::abs(orbit.y - camera.y - dm::radians(.5f)) < 1e-6f,
            "Legacy orbit sensitivity changed");
    gui.MouseButtonUpdate(GLFW_MOUSE_BUTTON_RIGHT, GLFW_RELEASE, 0);
    const auto released = view.cameraState();
    view.MousePosUpdate(130, 110);
    require(view.cameraState().x == released.x, "Inspector release left dragging active");
    view.MouseButtonUpdate(GLFW_MOUSE_BUTTON_MIDDLE, GLFW_PRESS, 0);
    const auto beforePan = view.cameraTarget();
    view.MousePosUpdate(140, 115);
    require(dm::length(view.cameraTarget() - beforePan) > 0, "Middle-button pan did not move target");
    view.MouseButtonUpdate(GLFW_MOUSE_BUTTON_MIDDLE, GLFW_RELEASE, 0);
    require(!gui.MouseScrollUpdate(0, 1), "Hidden SPH Inspector captured wheel");
    const float distance = view.cameraState().z;
    view.MouseScrollUpdate(0, 1);
    require(view.cameraState().z < distance, "Wheel did not zoom");
    require(draw() == 0, "Hidden SPH Inspector still draws");
    if (!view.paused())
        view.KeyboardUpdate(GLFW_KEY_SPACE, 0, GLFW_PRESS, 0);
    view.step(2);
    require(view.frame() == 2, "SPH fixture did not step");
    view.KeyboardUpdate(GLFW_KEY_R, 0, GLFW_PRESS, 0);
    require(view.frame() == 2, "Reset must be deferred out of event/UI handling");
    view.Animate(0);
    view.flush();
    require(view.frame() == 0 && view.simulation().parameters().integration.x == 0,
            "Paused reset did not restore simulation time");
    const auto restored = view.simulation().readPositions();
    view.step(1);
    view.KeyboardUpdate(GLFW_KEY_R, 0, GLFW_PRESS, 0);
    view.Animate(0);
    const auto repeated = view.simulation().readPositions();
    require(restored.size() == repeated.size() &&
                std::memcmp(restored.data(), repeated.data(), restored.size() * sizeof(g::Float4)) == 0,
            "Repeated reset changed initial particles");
    view.KeyboardUpdate(GLFW_KEY_GRAVE_ACCENT, 0, GLFW_PRESS, 0);
    require(draw() > 0, "SPH Inspector show failed");
    manager.GetDevice()->waitForIdle();
    std::puts("SPH Inspector geometry, capture, orbit/pan/zoom, pause and deferred-reset checks PASS");
}
void renderOffscreen(PoolView& view, nvrhi::IDevice* device, const std::string& path) {
    nvrhi::TextureDesc desc;
    desc.width = 640;
    desc.height = 480;
    desc.format = nvrhi::Format::RGBA8_UNORM;
    desc.isRenderTarget = true;
    desc.initialState = nvrhi::ResourceStates::RenderTarget;
    desc.keepInitialState = true;
    nvrhi::TextureHandle color;
    check(device->createTexture(desc, &color), "create particle render target");
    nvrhi::FramebufferHandle fb;
    check(device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(color), &fb),
          "create framebuffer");
    view.Render(fb);
    nvrhi::StagingTextureHandle staging;
    check(device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read, &staging), "create image readback");
    nvrhi::CommandListHandle commands;
    check(device->createCommandList(nvrhi::CommandListParameters(), &commands), "create readback commands");
    commands->open();
    commands->copyTexture2(staging, {}, color, {});
    commands->close();
    device->executeCommandList(commands);
    device->waitForIdle();
    size_t pitch = 0;
    const auto pixels =
        static_cast<const uint8_t*>(device->mapStagingTexture(staging, {}, nvrhi::CpuAccessMode::Read, pitch));
    if (!pixels)
        throw std::runtime_error("Cannot read particle image");
    size_t covered = 0;
    for (uint32_t y = 0; y < desc.height; ++y)
        for (uint32_t x = 0; x < desc.width; ++x) {
            const auto p = pixels + y * pitch + x * 4;
            if (p[0] > 20 || p[1] > 25 || p[2] > 35)
                ++covered;
        }
    const bool saved =
        path.empty() || stbi_write_png(path.c_str(), desc.width, desc.height, 4, pixels, int(pitch)) != 0;
    device->unmapStagingTexture(staging);
    if (!saved || !covered)
        throw std::runtime_error("Particle image is empty or cannot be written");
    std::printf("Rendered %zu particle pixels directly from GPU simulation buffers\n", covered);
}
}  // namespace
int main(int argc, const char** argv) {
    try {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const auto value = [&]() {
                if (++i >= argc)
                    throw std::invalid_argument("Missing option value");
                return argv[i];
            };
            if (!std::strcmp(argv[i], "--help")) {
                std::puts(
                    "sph_fluid_pool [-dx11|-dx12|-vk|-cuda] [--headless] [--adapter N] [--particles N] "
                    "[--example 0..3]\n"
                    "  [--frames N] [--check N] [--paused] [--screenshot image.png] [--output particles.csv]\n"
                    "  [--no-ui] Interactive: Space pause, R restart, Right/Alt+Left orbit, Middle pan, wheel "
                    "zoom, ` Inspector. CUDA runs headless.");
                return 0;
            }
            if (!std::strcmp(argv[i], "--particles"))
                options.capacity = uint32_t(std::stoul(value()));
            else if (!std::strcmp(argv[i], "--example"))
                options.example = std::stoi(value());
            else if (!std::strcmp(argv[i], "--adapter")) {
                options.adapter = std::stoi(value());
                if (options.adapter < 0)
                    throw std::invalid_argument("Negative adapter ordinal");
            } else if (!std::strcmp(argv[i], "--frames")) {
                options.steps = uint32_t(std::stoul(value()));
                options.bounded = true;
            } else if (!std::strcmp(argv[i], "--check"))
                options.checkStep = uint32_t(std::stoul(value()));
            else if (!std::strcmp(argv[i], "--headless"))
                options.headless = true;
            else if (!std::strcmp(argv[i], "--paused"))
                options.paused = true;
            else if (!std::strcmp(argv[i], "--no-ui"))
                options.showUI = false;
            else if (!std::strcmp(argv[i], "--gui-input-test")) {
                options.guiInputTest = true;
                options.bounded = true;
                options.steps = 2;
            } else if (!std::strcmp(argv[i], "--validation"))
                options.validation = true;
            else if (!std::strcmp(argv[i], "-cuda"))
                options.cuda = true;
            else if (!std::strcmp(argv[i], "--screenshot")) {
                options.screenshot = value();
                options.headless = true;
            } else if (!std::strcmp(argv[i], "--output"))
                options.csv = value();
        }
        if (options.example < 0 || options.example > 3 || options.steps > 1000000 ||
            (!options.headless && options.bounded && !options.steps))
            throw std::invalid_argument("Invalid preset or frame count");
        if (options.guiInputTest && (options.cuda || options.headless || !options.showUI))
            throw std::invalid_argument("GUI test requires a graphics window and Inspector");
        if (options.cuda) {
            if (!options.screenshot.empty())
                throw std::invalid_argument("CUDA has no rasterizer; select a graphics API for screenshots");
            es::ExampleDevice device(argc, argv);
            es::ComputeRuntime runtime(device.get(), device.shaderPath());
            sph::PortablePool pool(options.capacity, sph::SPHExample(options.example));
            g::SlangSphSimulation simulation(runtime, pool.parameters, pool.positions);
            if (!options.paused)
                simulation.step(options.steps);
            report(simulation);
            writeCsv(simulation, options.csv);
            return 0;
        }
        log::ConsoleApplicationMode();
        auto manager =
            nvrhi::TakeOver(app::DeviceManager::Create(app::GetGraphicsAPIFromCommandLine(argc, argv)));
        if (!manager)
            throw std::runtime_error("Requested graphics API is unavailable");
        ShutdownDevice shutdown{manager.Get()};
        app::DeviceCreationParameters p;
        p.enableNvrhiValidationLayer = true;
        p.enableDebugRuntime = options.validation;
        p.adapterIndex = options.adapter;
        const bool created = options.headless
                                 ? manager->CreateHeadlessDevice(p)
                                 : manager->CreateWindowDeviceAndSwapChain(p, "Slang SPH wave pool");
        if (!created)
            throw std::runtime_error("Cannot create graphics device");
        {
            PoolView view(manager, options);
            RegisteredPasses lifetime{manager.Get(), nullptr, nullptr};
            if (options.headless) {
                if (!options.paused)
                    view.step(options.steps);
                report(view.simulation());
                renderOffscreen(view, manager->GetDevice(), options.screenshot);
            } else {
                view.flush();
                auto fs = MAKE_RC_OBJ_PTR(vfs::RootFileSystem);
                fs->mount("donut", app::GetDirectoryWithExecutable() / "shaders/framework" /
                                       app::GetShaderTypeName(manager->GetDevice()->getGraphicsAPI()));
                engine::ShaderFactory shaders(manager->GetDevice(), fs, "");
                nvrhi::AutoPtr<PoolGuiPass> gui;
                if (options.showUI) {
                    gui = MAKE_RC_OBJ_PTR(PoolGuiPass, manager.Get(), view);
                    if (!gui->Init(&shaders))
                        throw std::runtime_error("Cannot initialize SPH Inspector");
                    ImGui::GetIO().IniFilename = nullptr;
                }
                RegisteredPasses registered{manager.Get(), &view, gui.Get()};
                manager->AddRenderPassToBack(&view);
                if (gui)
                    manager->AddRenderPassToBack(gui.Get());
                if (options.guiInputTest)
                    exerciseGui(view, *gui, *manager);
                manager->RunMessageLoop();
            }
            writeCsv(view.simulation(), options.csv);
            manager->GetDevice()->waitForIdle();
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "sph_fluid_pool: %s\n", error.what());
        return 1;
    }
}

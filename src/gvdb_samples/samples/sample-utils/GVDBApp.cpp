// Application base of the interactive GVDB samples.
#include "GVDBApp.h"
#include "ImageIO.h"
#include <donut/core/log.h>
#include <donut/core/math/math.h>
#include <nvrhi/utils.h>
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace SampleUtils {

using namespace donut;

// ----------------------------------------------------------------------------
// OrbitController (Camera3D::setOrbit / setAngles / moveRelative of the reference)
// ----------------------------------------------------------------------------

static dm::float3 orbitDirection(dm::float3 anglesDeg) {
    float ax = dm::radians(anglesDeg.x), ay = dm::radians(anglesDeg.y);
    return dm::float3(cosf(ay) * sinf(ax), sinf(ay), cosf(ay) * cosf(ax));
}

dm::float3 OrbitController::getPosition() const { return target + orbitDirection(angles) * distance; }

void OrbitController::setOrbit(dm::float3 anglesDeg, dm::float3 targetPos, float dist) {
    angles = anglesDeg;
    target = targetPos;
    distance = dist;
}

void OrbitController::setAngles(dm::float3 anglesDeg) {
    dm::float3 position = getPosition();
    angles = anglesDeg;
    target = position - orbitDirection(angles) * distance;
}

void OrbitController::setPosition(dm::float3 position) {
    dm::float3 d = position - target;
    float len = dm::length(d);
    if (len <= 1e-6f) return;
    distance = len;
    d /= len;
    angles = dm::float3(dm::degrees(atan2f(d.x, d.z)), dm::degrees(asinf(dm::clamp(d.y, -1.f, 1.f))), 0.f);
}

// Camera basis of the orbit: looks from position to target, +Y up.
static void orbitBasis(dm::float3 anglesDeg, dm::float3 &right, dm::float3 &up, dm::float3 &back) {
    back = orbitDirection(anglesDeg);   // from target to eye = camera backward axis
    dm::float3 worldUp(0.f, 1.f, 0.f);
    if (fabsf(dm::dot(back, worldUp)) > 0.999f) worldUp = dm::float3(0.f, 0.f, 1.f);
    right = dm::normalize(dm::cross(worldUp, back));
    up = dm::normalize(dm::cross(back, right));
}

void OrbitController::moveRelative(float dx, float dy, float dz) {
    dm::float3 right, up, back;
    orbitBasis(angles, right, up, back);
    dm::float3 v = right * dx + up * dy + back * dz;
    target += v;
}

void OrbitController::apply(engine::SceneGraphNode *node) const {
    if (!node) return;
    dm::float3 right, up, back;
    orbitBasis(angles, right, up, back);
    // Node space of a Donut camera: +X right, +Y up, -Z forward (row-vector
    // convention: rows are the images of the basis vectors).
    dm::float3x3 rot(right, up, back);
    dm::quat q;
    dm::float3 t, s;
    dm::decomposeAffine(dm::affine3(rot, dm::float3::zero()), &t, &q, &s);
    dm::double3 translation(getPosition());
    dm::dquat rotation(q);
    node->SetTransform(&translation, &rotation, nullptr);
}

// ----------------------------------------------------------------------------
// ImGui pass: one "Inspector" window built by the app
// ----------------------------------------------------------------------------

class GVDBAppGuiPass : public app::ImGuiRenderPass {
    NVRHI_INHERIT_INTERFACE_TABLE()
 public:
    GVDBAppGuiPass(app::DeviceManager *deviceManager, GVDBApp *app) : ImGuiRenderPass(deviceManager), m_app(app) {}

 protected:
    void BuildUI() override {
        ImGui::SetNextWindowPos(ImVec2(10.f, 10.f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(320.f, 0.f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Inspector")) {
            m_app->buildUI();
        }
        ImGui::End();
    }

 private:
    GVDBApp *m_app;
};

// ----------------------------------------------------------------------------
// GVDBApp
// ----------------------------------------------------------------------------

GVDBApp::GVDBApp(app::DeviceManager *deviceManager) : IRenderPass(deviceManager) {}

GVDBApp::~GVDBApp() {
    syncQueue(m_gpDevice, m_gpQueue);
    if (GetDevice()) GetDevice()->waitForIdle();
    m_debugDraw.reset();
    m_presenter.reset();
}

bool GVDBApp::Init(bool wantOptix) {
    // ---- file system ----
    m_vfs = MAKE_RC_OBJ_PTR(vfs::RootFileSystem);
    std::filesystem::path exeDir = app::GetDirectoryWithExecutable();
    const char *apiName = app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
    // The sub-directories first: RootFileSystem refuses to mount below an
    // existing mount point and resolves in mount order. "shaders/donut" is
    // where Donut's own passes (ImGui) look for their shaders.
    m_vfs->mount("shaders/donut", exeDir / "shaders" / "framework" / apiName);
    if (!m_sampleName.empty()) {
        std::filesystem::path sampleShaders = exeDir / "shaders" / m_sampleName / apiName;
        if (std::filesystem::exists(sampleShaders)) m_vfs->mount("shaders/" + m_sampleName, sampleShaders);
    }
    m_vfs->mount("shaders", exeDir / "shaders" / "gvdb_sample_utils" / apiName);
    m_vfs->mount("assets", std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR));
    m_vfs->mount("ptx", exeDir / "ptx");
    m_shaderFactory = MAKE_RC_OBJ_PTR(engine::ShaderFactory, GetDevice(), m_vfs, "shaders");

    // ---- devices ----
    {
        gp::CUDADeviceDesc desc;
        auto callback = MAKE_RC_OBJ_PTR(GPDeviceMessageCallback);
        desc.messageCallback = callback;
        if (NVRHI_FAILED(gp::createCUDADevice(desc, &m_gpDevice)) || !m_gpDevice) {
            log::error("GVDBApp: cannot create the CUDA device");
            return false;
        }
        gp::DeviceQueueDesc queueDesc;
        queueDesc.priority = gp::DeviceQueuePriority::AboveNormal;
        if (NVRHI_FAILED(m_gpDevice->createDeviceQueue(queueDesc, &m_gpQueue)) || !m_gpQueue) {
            log::error("GVDBApp: cannot create the CUDA queue");
            return false;
        }
        if (NVRHI_FAILED(createGPAndNVRHIDevice(GetDevice(), m_gpDevice, &m_interopDevice)) || !m_interopDevice) {
            log::error("GVDBApp: cannot create the nvrhi <-> CUDA interop device");
            return false;
        }
        if (wantOptix) {
            gp::RTDeviceDesc rtDesc;
            if (NVRHI_FAILED(gp::createOptiXDevice(rtDesc, m_gpDevice, &m_rtDevice)) || !m_rtDevice) {
                log::warning("GVDBApp: OptiX device not available; continuing without it");
                m_rtDevice = nullptr;
            }
        }
    }

    // ---- scene graph: camera + light leaves under their own nodes ----
    m_sceneGraph = MAKE_RC_OBJ_PTR(gvdb::GVDBSceneGraph);
    auto root = MAKE_RC_OBJ_PTR(engine::SceneGraphNode);
    root->SetName("root");
    m_sceneGraph->SetRootNode(root);

    m_camera = MAKE_RC_OBJ_PTR(engine::PerspectiveCamera);
    m_camera->zNear = 0.1f;
    m_camera->zFar = 5000.f;
    m_camera->verticalFov = dm::radians(50.f);
    m_cameraNode = m_sceneGraph->AttachLeafNode(root, m_camera);
    m_cameraNode->SetName("camera");

    auto pointLight = MAKE_RC_OBJ_PTR(engine::PointLight);
    m_light = pointLight;
    m_lightNode = m_sceneGraph->AttachLeafNode(root, m_light);
    m_lightNode->SetName("light");

    m_cameraOrbit.setOrbit(dm::float3(0.f, 45.f, 0.f), dm::float3::zero(), 120.f);
    m_lightOrbit.setOrbit(dm::float3(0.f, 45.f, 0.f), dm::float3::zero(), 120.f);

    // ---- presentation ----
    int w = 0, h = 0;
    GetDeviceManager()->GetWindowDimensions(w, h);
    nvrhi::IFramebuffer *fb = GetDeviceManager()->GetFramebuffer(0);
    const nvrhi::FramebufferInfoEx &fbInfo = fb->getFramebufferInfo();
    w = int(fbInfo.width);
    h = int(fbInfo.height);
    m_presenter = std::make_unique<ScreenPresenter>(GetDevice(), m_interopDevice, m_gpDevice, m_gpQueue, m_shaderFactory,
                                                    fbInfo);
    m_presenter->createTarget(uint32_t(w), uint32_t(h));
    m_debugDraw = std::make_unique<DebugDraw>(GetDevice(), m_shaderFactory, fbInfo);
    GetDevice()->createCommandList(nvrhi::CommandListParameters(), &m_commandList);

    updateScene();
    if (!OnInit()) return false;
    updateScene();
    return true;
}

void GVDBApp::updateScene() {
    m_cameraOrbit.apply(m_cameraNode);
    m_lightOrbit.apply(m_lightNode);
    m_sceneGraph->Refresh(m_frameCounter);
}

RenderView GVDBApp::getRenderView(int width, int height) const {
    return makeRenderView(m_camera, m_light, width, height);
}

nvrhi::AutoPtr<gvdb::IGVDBVolume> GVDBApp::createVolume() {
    nvrhi::AutoPtr<gvdb::IGVDBVolume> volume;
    if (NVRHI_FAILED(gvdb::createGVDBVolume(m_gpQueue, m_vfs, &volume))) {
        log::error("createGVDBVolume failed");
        return nullptr;
    }
    return volume;
}

nvrhi::AutoPtr<gvdb::GVDBVolumeInstance> GVDBApp::addVolumeInstance(gvdb::IGVDBVolume *volume,
                                                                   const dm::affine3 &indexToWorld,
                                                                   const std::string &name) {
    return attachVolumeInstance(m_sceneGraph, volume, indexToWorld, name);
}

std::filesystem::path GVDBApp::getAssetPath(const char *name) const {
    return std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR) / name;
}

void GVDBApp::OnMouseDrag(int button, int dx, int dy, int mods) {
    bool shift = (mods & GLFW_MOD_SHIFT) != 0;
    OrbitController &cam = m_cameraOrbit;
    OrbitController &orbit = shift ? m_lightOrbit : m_cameraOrbit;
    switch (button) {
        case GLFW_MOUSE_BUTTON_LEFT: {
            dm::float3 angles = orbit.angles;
            angles.x += float(dx) * 0.2f;
            angles.y -= float(dy) * 0.2f;
            orbit.setOrbit(angles, orbit.target, orbit.distance);
        } break;
        case GLFW_MOUSE_BUTTON_MIDDLE:
            cam.moveRelative(float(dx) * cam.distance / 1000.f, float(-dy) * cam.distance / 1000.f, 0.f);
            break;
        case GLFW_MOUSE_BUTTON_RIGHT: {
            float dist = orbit.distance - float(dy);
            orbit.setOrbit(orbit.angles, orbit.target, dist);
        } break;
        default: break;
    }
}

void GVDBApp::Render(nvrhi::IFramebuffer *framebuffer) {
    const nvrhi::FramebufferInfoEx &fbInfo = framebuffer->getFramebufferInfo();
    m_frameCounter++;
    updateScene();

    if (m_presenter->getWidth(0) != fbInfo.width || m_presenter->getHeight(0) != fbInfo.height)
        m_presenter->resizeTarget(0, fbInfo.width, fbInfo.height);

    // gp side
    m_presenter->beginGPFrame();
    OnRender(fbInfo.width, fbInfo.height);
    m_presenter->endGPFrame();

    // graphics side
    m_commandList->open();
    m_presenter->beginGraphics(m_commandList);
    m_presenter->blit(m_commandList, framebuffer, 0);
    OnDrawOverlay(m_commandList, framebuffer);
    if (m_debugDraw->getLineCount() > 0) {
        RenderView view = getRenderView(int(fbInfo.width), int(fbInfo.height));
        m_debugDraw->flush(m_commandList, framebuffer, view.viewProj);
    }
    m_presenter->endGraphics(m_commandList);
    m_commandList->close();
    GetDevice()->executeCommandList(m_commandList);
    m_presenter->endGraphicsFrame();

    if (!m_screenshotPath.empty() && m_frameCounter >= m_screenshotFrames) captureScreenshot(framebuffer);
}

// Reads colour attachment 0 of the presented framebuffer back through a staging
// texture (RGBA8 / BGRA8 only, which is what the swap chain uses) and saves it.
void GVDBApp::captureScreenshot(nvrhi::IFramebuffer *framebuffer) {
    std::filesystem::path path = m_screenshotPath;
    m_screenshotPath.clear();
    nvrhi::IDevice *device = GetDevice();
    device->waitForIdle();

    nvrhi::ITexture *texture = framebuffer->getDesc().colorAttachments[0].texture;
    nvrhi::TextureDesc desc = texture->getDesc();
    bool bgra = false;
    switch (desc.format) {
        case nvrhi::Format::RGBA8_UNORM:
        case nvrhi::Format::SRGBA8_UNORM: break;
        case nvrhi::Format::BGRA8_UNORM:
        case nvrhi::Format::SBGRA8_UNORM: bgra = true; break;
        default:
            log::error("Screenshot: unsupported back buffer format %d", int(desc.format));
            glfwSetWindowShouldClose(GetDeviceManager()->GetWindow(), GLFW_TRUE);
            return;
    }

    nvrhi::StagingTextureHandle staging;
    device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read, &staging);
    nvrhi::CommandListHandle commandList;
    device->createCommandList(nvrhi::CommandListParameters(), &commandList);
    commandList->open();
    commandList->beginTrackingTextureState(texture, nvrhi::TextureSubresourceSet(0, 1, 0, 1),
                                           nvrhi::ResourceStates::Present);
    commandList->copyTexture2(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice());
    commandList->setTextureState(texture, nvrhi::TextureSubresourceSet(0, 1, 0, 1), nvrhi::ResourceStates::Present);
    commandList->commitBarriers();
    commandList->close();
    device->executeCommandList(commandList);

    size_t rowPitch = 0;
    const uint8_t *data = static_cast<const uint8_t *>(
        device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, rowPitch));
    bool ok = false;
    if (data) {
        std::vector<uint8_t> rgba(size_t(desc.width) * desc.height * 4);
        for (uint32_t y = 0; y < desc.height; ++y) {
            const uint8_t *src = data + size_t(y) * rowPitch;
            uint8_t *dst = rgba.data() + size_t(y) * desc.width * 4;
            for (uint32_t x = 0; x < desc.width; ++x, src += 4, dst += 4) {
                dst[0] = bgra ? src[2] : src[0];
                dst[1] = src[1];
                dst[2] = bgra ? src[0] : src[2];
                dst[3] = 255;
            }
        }
        device->unmapStagingTexture(staging);
        ok = savePNG(path, rgba.data(), desc.width, desc.height);
    }
    if (ok)
        log::info("Saved %s (frame %u)", path.string().c_str(), m_frameCounter);
    else
        log::error("Cannot write %s", path.string().c_str());
    glfwSetWindowShouldClose(GetDeviceManager()->GetWindow(), GLFW_TRUE);
}

void GVDBApp::Animate(float elapsedTimeSeconds) { OnUpdate(elapsedTimeSeconds); }

void GVDBApp::BackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) {
    if (m_presenter) m_presenter->resizeTarget(0, width, height);
    OnResize(width, height);
}

bool GVDBApp::KeyboardUpdate(int key, int scancode, int action, int mods) {
    m_mouseMods = mods;
    return OnKey(key, scancode, action, mods);
}

bool GVDBApp::MousePosUpdate(double xpos, double ypos) {
    dm::int2 pos{int(xpos), int(ypos)};
    dm::int2 delta = pos - m_mousePos;
    m_mousePos = pos;
    if (m_mouseDown >= 0 && (delta.x != 0 || delta.y != 0)) OnMouseDrag(m_mouseDown, delta.x, delta.y, m_mouseMods);
    return false;
}

bool GVDBApp::MouseButtonUpdate(int button, int action, int mods) {
    m_mouseMods = mods;
    if (action == GLFW_PRESS)
        m_mouseDown = button;
    else if (button == m_mouseDown)
        m_mouseDown = -1;
    return OnMouseButton(button, action, mods);
}

// ----------------------------------------------------------------------------
// runGVDBApp
// ----------------------------------------------------------------------------

int runGVDBApp(int argc, const char *const *argv, const GVDBAppOptions &options,
               const std::function<nvrhi::AutoPtr<GVDBApp>(app::DeviceManager *)> &factory) {
    log::EnableOutputToConsole(true);
    log::EnableOutputToDebug(true);
    log::EnableOutputToMessageBox(false);
    setvbuf(stdout, nullptr, _IONBF, 0);   // console samples: keep the log readable when redirected

    bool debug = false;
    std::filesystem::path screenshot;
    uint32_t screenshotFrames = 8;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-debug") == 0) debug = true;
        else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) screenshot = argv[++i];
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) screenshotFrames = uint32_t(atoi(argv[++i]));
    }

    nvrhi::GraphicsAPI api = app::GetGraphicsAPIFromCommandLine(argc, argv);
    auto deviceManager = nvrhi::TakeOver(app::DeviceManager::Create(api));
    if (!deviceManager) {
        log::error("Cannot create the device manager for the requested API");
        return 1;
    }

    app::DeviceCreationParameters params;
    params.backBufferWidth = options.width;
    params.backBufferHeight = options.height;
    params.swapChainFormat = nvrhi::Format::RGBA8_UNORM;
    params.vsyncEnabled = options.vsync;
    params.enableDebugRuntime = debug;
    params.enableNvrhiValidationLayer = debug;
    if (api == nvrhi::GraphicsAPI::VULKAN) {
        // CUDA interop needs exportable memory / timeline semaphores (opaque Win32 handles).
        params.requiredVulkanDeviceExtensions = {"VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32"};
    }
    if (!deviceManager->CreateWindowDeviceAndSwapChain(params, options.title)) {
        log::error("Cannot initialize a graphics device with the requested parameters");
        return 1;
    }
    log::info("%s: %s", options.title, deviceManager->GetRendererString());

    int result = 0;
    {
        nvrhi::AutoPtr<GVDBApp> appPass = factory(deviceManager);
        if (appPass && options.sampleName) appPass->setSampleName(options.sampleName);
        if (appPass && !screenshot.empty()) appPass->setScreenshot(screenshot, screenshotFrames);
        if (!appPass || !appPass->Init(options.wantOptix)) {
            log::error("%s: initialisation failed", options.title);
            result = 1;
        } else {
            auto gui = MAKE_RC_OBJ_PTR(GVDBAppGuiPass, deviceManager.Get(), appPass.Get());
            gui->Init(appPass->getShaderFactory());
            deviceManager->AddRenderPassToBack(appPass);
            deviceManager->AddRenderPassToBack(gui);
            deviceManager->RunMessageLoop();
            deviceManager->RemoveRenderPass(gui);
            deviceManager->RemoveRenderPass(appPass);
            deviceManager->GetDevice()->waitForIdle();
        }
    }
    deviceManager->Shutdown();
    return result;
}

}  // namespace SampleUtils

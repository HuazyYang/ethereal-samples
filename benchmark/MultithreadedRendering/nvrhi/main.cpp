// mt_rendering_nvrhi: the MultithreadedRendering workload rendered with Donut's DeviceManager and nvrhi
// on D3D12 or Vulkan. Benchmark contract: common/benchmark.h.
//
//   mt_rendering_nvrhi -benchmark -renderer nvrhi -api d3d12 -mode mt_chunk -threads 4 -output DIR
//
// Without -benchmark the window stays open (defaults: -api d3d12, -mode st or mt_chunk with -threads > 1).

#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/ShaderFactory.h>

#include <cstdio>
#include <cstring>
#include <string>

#include <mmsystem.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "renderer.h"

using namespace MtScene;

namespace
{

void LogCallback(donut::log::Severity severity, const char* text)
{
    using donut::log::Severity;
    if (severity >= Severity::Warning)
    {
        fprintf(stderr, "[donut %s] %s\n", severity == Severity::Warning ? "warning" : "error", text);
        fflush(stderr);
    }
    else
        printf("[donut] %s\n", text);
    if (severity >= Severity::Error && Benchmark::config.enabled)
        Benchmark::Fail(std::string("Donut/nvrhi error: ") + text);
}

void PrintUsage()
{
    fprintf(stderr, "usage: mt_rendering_nvrhi [options]\n%s"
                    "  -mirror_scissor            tight stencil scissor around each mirror (the sample's option)\n"
                    "  -explicit_barriers         nvrhi automatic barriers off; transitions placed where the native renderer places them\n"
                    "  -debug                     D3D12 debug layer / Vulkan validation and the nvrhi validation layer\n"
                    "  -media DIR                 directory holding SquidRoom/SquidRoom.sdkmesh (default: <exe dir>/../media)\n"
                    "  -freeze T                  render the scripted frame at time T seconds (parity screenshots)\n"
                    "  -screenshot FILE.bmp       save the 30th frame as a BMP and exit\n",
            Benchmark::Usage());
}

bool QueryDeviceLuid(donut::app::DeviceManager* deviceManager, uint64_t& luid, std::string& error)
{
    nvrhi::IDevice* device = deviceManager->GetDevice();
    switch (device->getGraphicsAPI())
    {
        case nvrhi::GraphicsAPI::D3D12:
        {
            ID3D12Device* d3d12 = static_cast<ID3D12Device*>(device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device));
            if (!d3d12) { error = "cannot query the D3D12 device"; return false; }
            luid = Benchmark::LuidToU64(d3d12->GetAdapterLuid());
            return true;
        }
        case nvrhi::GraphicsAPI::VULKAN:
        {
            VkPhysicalDevice physicalDevice = static_cast<VkPhysicalDevice>(device->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice));
            std::vector<donut::app::AdapterInfo> adapters;
            deviceManager->EnumerateAdapters(adapters);
            for (const auto& adapter : adapters)
            {
                if (adapter.vkPhysicalDevice != physicalDevice) continue;
                if (!adapter.luid) { error = "the Vulkan physical device '" + adapter.name + "' reports no LUID"; return false; }
                luid = Benchmark::LuidFromBytes(adapter.luid->data());
                return true;
            }
            error = "cannot identify the Vulkan physical device";
            return false;
        }
        default:
            error = "unsupported graphics API";
            return false;
    }
}

} // namespace

int main(int argc, char** argv)
{
    Benchmark::Config& config = Benchmark::config;
    bool debug = false, tightScissor = false, explicitBarriers = false;
    double freezeTime = -1.0;
    std::filesystem::path screenshot;
    std::filesystem::path mediaDir = donut::app::GetDirectoryWithExecutable().parent_path() / "media";

    for (int a = 1; a + 1 < argc; ++a)
        if (_stricmp(argv[a], "-output") == 0) config.output = argv[a + 1];

    for (int a = 1; a < argc; ++a)
    {
        std::string error;
        const Benchmark::ArgResult result = Benchmark::ParseArg(argc, argv, a, config, error);
        if (result == Benchmark::ArgResult::Consumed) continue;
        if (result == Benchmark::ArgResult::Error) { PrintUsage(); Benchmark::Fail(error); }
        if (_stricmp(argv[a], "-debug") == 0) debug = true;
        else if (_stricmp(argv[a], "-mirror_scissor") == 0) tightScissor = true;
        else if (_stricmp(argv[a], "-explicit_barriers") == 0) explicitBarriers = true;
        else if (_stricmp(argv[a], "-media") == 0 && a + 1 < argc) mediaDir = argv[++a];
        else if (_stricmp(argv[a], "-freeze") == 0 && a + 1 < argc) freezeTime = atof(argv[++a]);
        else if (_stricmp(argv[a], "-screenshot") == 0 && a + 1 < argc) screenshot = argv[++a];
        else if (_stricmp(argv[a], "-help") == 0 || _stricmp(argv[a], "-h") == 0 || _stricmp(argv[a], "-?") == 0) { PrintUsage(); return 0; }
        else { PrintUsage(); Benchmark::Fail(std::string("unrecognized argument '") + argv[a] + "'"); }
    }

    if (!config.enabled)
    {
        if (config.renderer.empty()) config.renderer = "nvrhi";
        if (config.api.empty()) config.api = "d3d12";
        if (config.mode.empty()) config.mode = config.threads > 1 ? "mt_chunk" : "st";
    }
    static const char* const kRenderers[] = { "nvrhi" };
    Benchmark::ValidateOrFail(config, kRenderers, 1);
    if (config.renderer != "nvrhi") Benchmark::Fail("this executable only implements -renderer nvrhi", Benchmark::FailKind::Unsupported);
    {
        std::string reason;
        if (!Benchmark::CombinationSupported(config.renderer, config.api, config.mode, config.threads, reason))
            Benchmark::Fail(reason, Benchmark::FailKind::Unsupported);
    }
    Mode mode;
    ParseMode(config.mode, mode);
    const nvrhi::GraphicsAPI api = config.api == "vk" ? nvrhi::GraphicsAPI::VULKAN : nvrhi::GraphicsAPI::D3D12;

    donut::log::ConsoleApplicationMode();
    donut::log::SetCallback(LogCallback);
    donut::log::SetMinSeverity(donut::log::Severity::Warning);
    timeBeginPeriod(1);

    // ---- scene ----
    SdkMesh mesh;
    {
        std::string error;
        if (!mesh.Load(mediaDir / kMeshFile, error))
            Benchmark::Fail(error);
    }
    const uint64_t sceneHash = mesh.ContentHash();
    printf("scene_hash %s (%u meshes, %u materials)\n", Benchmark::Hex64(sceneHash).c_str(), mesh.NumMeshes(), mesh.NumMaterials());
    Scene scene;
    scene.SetTightMirrorScissor(tightScissor);

    // ---- device ----
    donut::app::DeviceCreationParameters deviceParams;
    deviceParams.backBufferWidth = uint32_t(config.width);
    deviceParams.backBufferHeight = uint32_t(config.height);
    deviceParams.swapChainBufferCount = 3;
    deviceParams.maxFramesInFlight = 3;
    deviceParams.swapChainFormat = nvrhi::Format::SRGBA8_UNORM;   // the sample's sRGB back buffer
    deviceParams.depthBufferFormat = nvrhi::Format::D24S8;        // the sample's depth/stencil buffer
    deviceParams.vsyncEnabled = false;
    deviceParams.enableDebugRuntime = debug;
    deviceParams.enableNvrhiValidationLayer = debug;
    deviceParams.enablePerMonitorDPI = true;
    deviceParams.resizeWindowWithDisplayScale = false;

    nvrhi::AutoPtr<donut::app::DeviceManager> deviceManager = nvrhi::TakeOver(donut::app::DeviceManager::Create(api));
    if (!deviceManager)
        Benchmark::Fail("Donut has no device manager for -api " + config.api);
    if (!deviceManager->CreateInstance(deviceParams))
        Benchmark::Fail("DeviceManager::CreateInstance failed for -api " + config.api);

    if (config.adapterLuid != 0)
    {
        std::vector<donut::app::AdapterInfo> adapters;
        if (!deviceManager->EnumerateAdapters(adapters))
            Benchmark::Fail("DeviceManager::EnumerateAdapters failed");
        for (size_t i = 0; i < adapters.size(); ++i)
            if (adapters[i].luid && Benchmark::LuidFromBytes(adapters[i].luid->data()) == config.adapterLuid)
            {
                deviceParams.adapterIndex = int(i);
                break;
            }
        if (deviceParams.adapterIndex < 0)
            Benchmark::Fail("no " + config.api + " adapter with LUID " + std::to_string(config.adapterLuid));
    }

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, "MultithreadedRendering (nvrhi)"))
        Benchmark::Fail("DeviceManager::CreateWindowDeviceAndSwapChain failed for -api " + config.api);
    if (config.enabled) Benchmark::KeepWindowOnTop(glfwGetWin32Window(deviceManager->GetWindow()));

    {
        uint64_t luid = 0;
        std::string error;
        if (!QueryDeviceLuid(deviceManager, luid, error))
            Benchmark::Fail(error);
        Benchmark::SetActualAdapterOrFail(luid);
        printf("adapter %s (LUID %llu)\n", deviceManager->GetRendererString(), static_cast<unsigned long long>(luid));
    }

    // ---- shaders and renderer ----
    nvrhi::IDevice* device = deviceManager->GetDevice();
    const std::filesystem::path exeDirectory = donut::app::GetDirectoryWithExecutable();
    const char* shaderTypeName = donut::app::GetShaderTypeName(device->getGraphicsAPI());
    auto rootFS = MAKE_RC_OBJ_PTR(donut::vfs::RootFileSystem);
    rootFS->mount("/shaders/donut", exeDirectory / "shaders/framework" / shaderTypeName);
    rootFS->mount("/shaders/app", exeDirectory / "shaders/mt_rendering_nvrhi" / shaderTypeName);
    auto shaderFactory = MAKE_RC_OBJ_PTR(donut::engine::ShaderFactory, device, rootFS.Get(), std::filesystem::path("/shaders"));

    RendererDesc rendererDesc;
    rendererDesc.mode = mode;
    rendererDesc.threads = uint32_t(config.threads);
    rendererDesc.tightMirrorScissor = tightScissor;
    rendererDesc.explicitBarriers = explicitBarriers;
    rendererDesc.debug = debug;
    rendererDesc.freezeTime = float(freezeTime);
    rendererDesc.screenshot = screenshot;

    auto renderer = MAKE_RC_OBJ_PTR(MtRenderer, deviceManager.Get(), rendererDesc, &scene);
    if (!renderer->Init(*shaderFactory, mesh))
        Benchmark::Fail("renderer initialization failed");

    deviceManager->AddRenderPassToBack(renderer);
    if (config.enabled)
        renderer->InstallFrameCallbacks();

    // ---- frame loop (Donut) ----
    deviceManager->RunMessageLoop();

    int exitCode = 0;
    if (config.enabled)
    {
        Benchmark::RunInfo info;
        renderer->FillRunInfo(info);
        if (info.width != config.width || info.height != config.height)
            Benchmark::Fail("back buffer is " + std::to_string(info.width) + "x" + std::to_string(info.height) + ", requested " +
                            std::to_string(config.width) + "x" + std::to_string(config.height));
        info.sceneHash = sceneHash;
        Benchmark::AddDesktopFacts(info, glfwGetWin32Window(deviceManager->GetWindow()));
        exitCode = Benchmark::Finish(info);
    }

    deviceManager->m_callbacks = {};
    renderer->Shutdown();
    deviceManager->RemoveRenderPass(renderer);
    renderer = nullptr;
    shaderFactory = nullptr;
    rootFS = nullptr;
    deviceManager->Shutdown();
    deviceManager = nullptr;
    timeEndPeriod(1);
    return exitCode;
}

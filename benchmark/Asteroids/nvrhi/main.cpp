// asteroids_nvrhi: the Asteroids workload (50,000 asteroids, 1000 meshes, 10 textures) rendered with
// Donut's DeviceManager and nvrhi on D3D11, D3D12 or Vulkan. Benchmark contract: docs/PLAN.md and
// src/common/benchmark.h.
//
//   asteroids_nvrhi -benchmark -renderer nvrhi -api d3d12 -binding tex_mut -threads 8
//                   -warmup 10 -duration 30 -adapter_luid N -output DIR
//
// Without -benchmark the window stays open until it is closed (defaults: -api d3d12 -binding tex_mut).

#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/ShaderFactory.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include <mmsystem.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "renderer.h"

using namespace DirectX;

namespace
{

// Every Donut/nvrhi error is fatal here: a benchmark run that logged an error must not report numbers.
void LogCallback(donut::log::Severity severity, const char* text)
{
    using donut::log::Severity;
    if (severity >= Severity::Warning)
    {
        fprintf(stderr, "[donut %s] %s\n", severity == Severity::Warning ? "warning" : "error", text);
        fflush(stderr);
    }
    else
    {
        printf("[donut] %s\n", text);
    }
    if (severity >= Severity::Error)
        Benchmark::Fail(std::string("Donut/nvrhi error: ") + text);
}

void PrintUsage()
{
    fprintf(stderr, "usage: asteroids_nvrhi [options]\n%s"
                    "sensitivity switches (defaults: redundant binding-set filter on, trackLiveness off, automatic barriers on):\n"
                    "  -track_liveness            create the binding sets with nvrhi's default trackLiveness = true\n"
                    "                             (default here: false, the sets outlive every command list)\n"
                    "  -always_set_state          call setGraphicsState before every draw\n"
                    "  -no_auto_barriers          setEnableAutomaticBarriers(false) after the first setGraphicsState of\n"
                    "                             each command list, re-enabled after the draw loop\n"
                    "placement of the nvrhi D3D12 command list objects (docs/LIMITATIONS.md, page-split store; d3d12 only;\n"
                    "default: whatever the heap hands out, recorded in run.json extra.cb_page_split):\n"
                    "  -cb_page_split_avoid       no page boundary inside the command lists' volatile constant buffer state\n"
                    "  -cb_page_split_force       a page boundary inside it, for every recording thread\n",
            Benchmark::Usage());
}

// LUID of the adapter the nvrhi device really runs on.
bool QueryDeviceLuid(donut::app::DeviceManager* deviceManager, uint64_t& luid, std::string& error)
{
    nvrhi::IDevice* device = deviceManager->GetDevice();
    switch (device->getGraphicsAPI())
    {
        case nvrhi::GraphicsAPI::D3D11:
        {
            ID3D11Device* d3d11 = device->getNativeObject(nvrhi::ObjectTypes::D3D11_Device);
            nvrhi::AutoPtr<IDXGIDevice> dxgiDevice;
            nvrhi::AutoPtr<IDXGIAdapter> adapter;
            DXGI_ADAPTER_DESC desc{};
            if (!d3d11 || FAILED(d3d11->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
                FAILED(adapter->GetDesc(&desc)))
            {
                error = "cannot query the adapter of the D3D11 device";
                return false;
            }
            luid = Benchmark::LuidToU64(desc.AdapterLuid);
            return true;
        }
        case nvrhi::GraphicsAPI::D3D12:
        {
            ID3D12Device* d3d12 = device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);
            if (!d3d12)
            {
                error = "cannot query the D3D12 device";
                return false;
            }
            luid = Benchmark::LuidToU64(d3d12->GetAdapterLuid());
            return true;
        }
        case nvrhi::GraphicsAPI::VULKAN:
        {
            VkPhysicalDevice physicalDevice = device->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice);
            std::vector<donut::app::AdapterInfo> adapters;
            deviceManager->EnumerateAdapters(adapters);
            for (const auto& adapter : adapters)
            {
                if (adapter.vkPhysicalDevice != physicalDevice) continue;
                if (!adapter.luid)
                {
                    error = "the Vulkan physical device '" + adapter.name + "' reports no LUID";
                    return false;
                }
                luid = Benchmark::LuidFromBytes(adapter.luid->data());
                return true;
            }
            error = "cannot identify the Vulkan physical device";
            return false;
        }
    }
    error = "unknown graphics API";
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    Benchmark::Config& config = Benchmark::config;
    bool trackLiveness = false, alwaysSetState = false, noAutoBarriers = false;
    RendererDesc::CbPageSplit cbPageSplit = RendererDesc::CbPageSplit::Natural;

    // ---- command line ----
    {
        std::string error;
        for (int a = 1; a < argc; ++a)
        {
            const Benchmark::ArgResult result = Benchmark::ParseArg(argc, argv, a, config, error);
            if (result == Benchmark::ArgResult::Consumed) continue;
            if (result == Benchmark::ArgResult::Error) Benchmark::Fail(error);

            if (_stricmp(argv[a], "-track_liveness") == 0)
                trackLiveness = true;
            else if (_stricmp(argv[a], "-always_set_state") == 0)
                alwaysSetState = true;
            else if (_stricmp(argv[a], "-no_auto_barriers") == 0)
                noAutoBarriers = true;
            else if (_stricmp(argv[a], "-cb_page_split_avoid") == 0)
                cbPageSplit = RendererDesc::CbPageSplit::Avoid;
            else if (_stricmp(argv[a], "-cb_page_split_force") == 0)
                cbPageSplit = RendererDesc::CbPageSplit::Force;
            else if (_stricmp(argv[a], "-help") == 0 || _stricmp(argv[a], "-h") == 0 || _stricmp(argv[a], "-?") == 0)
            {
                PrintUsage();
                return 0;
            }
            else
            {
                PrintUsage();
                Benchmark::Fail(std::string("unrecognized argument '") + argv[a] + "'");
            }
        }
    }

    if (!config.enabled)
    {
        // Interactive run: fill in defaults, then apply the same combination rules.
        if (config.renderer.empty()) config.renderer = "nvrhi";
        if (config.api.empty()) config.api = "d3d12";
        if (config.binding.empty()) config.binding = "tex_mut";
        std::string reason;
        if (config.renderer != "nvrhi") Benchmark::Fail("this executable only implements -renderer nvrhi", Benchmark::FailKind::Unsupported);
        if (!Benchmark::CombinationSupported(config.renderer, config.api, config.binding, reason))
            Benchmark::Fail(reason, Benchmark::FailKind::Unsupported);
    }
    static const char* const kRenderers[] = {"nvrhi"};
    Benchmark::ValidateOrFail(config, kRenderers, 1);

    nvrhi::GraphicsAPI api = nvrhi::GraphicsAPI::D3D12;
    if (config.api == "d3d11") api = nvrhi::GraphicsAPI::D3D11;
    else if (config.api == "vk") api = nvrhi::GraphicsAPI::VULKAN;

    RendererDesc rendererDesc;
    if (config.binding == "mut") rendererDesc.binding = BindingMode::Mut;
    else if (config.binding == "tex_mut") rendererDesc.binding = BindingMode::TexMut;
    else if (config.binding == "tex_mut_pc") rendererDesc.binding = BindingMode::TexMutPC;
    else if (config.binding == "bindless") rendererDesc.binding = BindingMode::Bindless;
    else Benchmark::Fail("binding '" + config.binding + "' is not expressible in nvrhi", Benchmark::FailKind::Unsupported);
    rendererDesc.threads       = uint32_t(config.threads);
    rendererDesc.trackLiveness  = trackLiveness;
    rendererDesc.alwaysSetState = alwaysSetState;
    rendererDesc.noAutoBarriers = noAutoBarriers;
    rendererDesc.gpuTiming     = config.gpuTiming;
    rendererDesc.cbPageSplit   = cbPageSplit;
    if (cbPageSplit != RendererDesc::CbPageSplit::Natural && api != nvrhi::GraphicsAPI::D3D12)
        Benchmark::Fail("-cb_page_split_avoid / -cb_page_split_force apply to the nvrhi D3D12 command list only (-api d3d12)",
                        Benchmark::FailKind::Unsupported);

    if (config.threads > 64)
        Benchmark::Fail("-threads must be between 1 and 64");
    if (api == nvrhi::GraphicsAPI::D3D11 && config.threads > 1)
    {
        // Donut/nvrhi/src/d3d11/d3d11-device.cpp, Device::createCommandList: a command list with
        // enableImmediateExecution = false is rejected ("Deferred command lists are not supported by the
        // D3D11 backend.") and every call returns the one m_ImmediateCommandList, whose methods all go to
        // Context::immediateContext; Device::executeCommandLists is an empty function (d3d11-backend.h).
        Benchmark::Fail("the nvrhi D3D11 backend cannot record on multiple threads: it has a single command list that "
                        "maps to the immediate context and no deferred command lists (-threads must be 1)",
                        Benchmark::FailKind::Unsupported);
    }
    if (api == nvrhi::GraphicsAPI::D3D11 && rendererDesc.binding == BindingMode::Bindless)
        Benchmark::Fail("bindless requires d3d12 or vk", Benchmark::FailKind::Unsupported);

    donut::log::ConsoleApplicationMode();
    donut::log::SetCallback(LogCallback);
    donut::log::SetMinSeverity(donut::log::Severity::Warning);

    timeBeginPeriod(1); // as the reference sample does

    // ---- scene: the reference sample's simulation with the contract's seed ----
    Settings settings;
    settings.windowWidth  = config.width;
    settings.windowHeight = config.height;
    settings.renderWidth  = config.width;
    settings.renderHeight = config.height;
    settings.numThreads   = config.threads;
    settings.animate      = true;
    settings.vsync        = false;
    settings.multithreadedRendering = config.threads > 1;

    AsteroidsSimulation simulation(Benchmark::kSeed, NUM_ASTEROIDS, NUM_UNIQUE_MESHES, MESH_MAX_SUBDIV_LEVELS, NUM_UNIQUE_TEXTURES);
    const uint64_t staticSceneHash = Benchmark::StaticSceneHash(simulation, unsigned(NUM_ASTEROIDS));
    printf("static_scene_hash %s\n", Benchmark::Hex64(staticSceneHash).c_str());

    // Camera of the reference (ResetCameraView in WinWrapper.cpp); the projection is set on resize.
    OrbitCamera camera;
    {
        const XMVECTOR center = XMVectorSet(0.0f, -0.4f * SIM_DISC_RADIUS, 0.0f, 0.0f);
        const float radius    = SIM_ORBIT_RADIUS + SIM_DISC_RADIUS + 10.f;
        const float minRadius = SIM_ORBIT_RADIUS - 3.0f * SIM_DISC_RADIUS;
        const float maxRadius = SIM_ORBIT_RADIUS + 3.0f * SIM_DISC_RADIUS;
        camera.View(center, radius, minRadius, maxRadius, 4.50f, 1.45f);
    }

    // ---- device ----
    donut::app::DeviceCreationParameters deviceParams;
    deviceParams.backBufferWidth      = uint32_t(config.width);
    deviceParams.backBufferHeight     = uint32_t(config.height);
    deviceParams.swapChainBufferCount = NUM_SWAP_CHAIN_BUFFERS;         // 5, as the reference
    deviceParams.maxFramesInFlight    = NUM_FRAMES_TO_BUFFER;           // 3; only Donut's Vulkan path uses it
    deviceParams.swapChainFormat      = nvrhi::Format::SRGBA8_UNORM;    // Diligent: RGBA8_UNORM_SRGB; Donut/Vulkan turns it into BGRA
    deviceParams.depthBufferFormat    = nvrhi::Format::UNKNOWN;         // the render pass owns the depth buffer
    deviceParams.vsyncEnabled         = false;
    deviceParams.enableDebugRuntime   = false;
    deviceParams.enableWarningsAsErrors = false;
    deviceParams.enableGPUValidation  = false;
    deviceParams.enableNvrhiValidationLayer = false;
    // DPI-aware process, window not scaled with the display scale: the client area and therefore the back
    // buffer are exactly -window W H physical pixels (the reference calls SetProcessDpiAwareness itself).
    deviceParams.enablePerMonitorDPI  = true;
    deviceParams.resizeWindowWithDisplayScale = false;
    deviceParams.infoLogSeverity      = donut::log::Severity::Debug;

    nvrhi::AutoPtr<donut::app::DeviceManager> deviceManager = nvrhi::TakeOver(donut::app::DeviceManager::Create(api));
    if (!deviceManager)
        Benchmark::Fail("Donut has no device manager for -api " + config.api);

    if (!deviceManager->CreateInstance(deviceParams))
        Benchmark::Fail("DeviceManager::CreateInstance failed for -api " + config.api);

    if (config.adapterLuid != 0)
    {
        // Pin the adapter: DeviceCreationParameters::adapterIndex follows the order of EnumerateAdapters.
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

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, "asteroids_nvrhi"))
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
    rootFS->mount("/shaders/app", exeDirectory / "shaders/asteroids_nvrhi" / shaderTypeName);
    auto shaderFactory = MAKE_RC_OBJ_PTR(donut::engine::ShaderFactory, device, rootFS.Get(), std::filesystem::path("/shaders"));

    auto renderer = MAKE_RC_OBJ_PTR(AsteroidsRenderer, deviceManager.Get(), rendererDesc, &simulation, &camera, &settings);
    if (!renderer->Init(*shaderFactory, exeDirectory / "media"))
        Benchmark::Fail("renderer initialization failed");

    deviceManager->AddRenderPassToBack(renderer);
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
        Benchmark::AddDesktopFacts(info, glfwGetWin32Window(deviceManager->GetWindow()));
        info.staticSceneHash       = staticSceneHash;
        info.finalDynamicSceneHash = Benchmark::DynamicSceneHash(simulation, unsigned(NUM_ASTEROIDS));
        exitCode = Benchmark::Finish(info);
    }

    // ---- shutdown ----
    deviceManager->m_callbacks = {};
    renderer->Shutdown();
    deviceManager->RemoveRenderPass(renderer);
    renderer      = nullptr;
    shaderFactory = nullptr;
    rootFS        = nullptr;
    deviceManager->Shutdown();
    deviceManager = nullptr;
    timeEndPeriod(1);
    return exitCode;
}

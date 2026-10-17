#include <Windows.h>
#include <crtdbg.h>

#include <donut/app/DeviceManager.h>
#include <waveworks/GFSDK_WaveWorks.h>

#include "example_app.h"
#include "asset_utils.h"
#include "main_state.h"
#include "dxgi_adapter_selector.h"
#include "example_options.h"
#include "ocean_renderer.h"
#include "sky_renderer.h"
#include "ui_renderer.h"

#include <cstdio>
#include <donut/core/log.h>
#include <vulkan/vulkan.h>
#include <cstring>
#include <vector>
#include <string>

namespace
{
MainState g_MainState;
nvrhi::AutoPtr<donut::app::DeviceManager> g_DeviceManager;
nvrhi::AutoPtr<UIRenderer> g_UIRenderer;
nvrhi::AutoPtr<SkyRenderer> g_SkyRenderer;
nvrhi::AutoPtr<OceanRenderer> g_OceanRenderer;
nvrhi::AutoPtr<IExampleScenePipeline> g_ScenePipeline;
bool g_WaveWorksInitialized = false;

const char *BackendName(nvrhi::GraphicsAPI backend)
{
    switch (backend)
    {
    case nvrhi::GraphicsAPI::D3D11: return "D3D11";
    case nvrhi::GraphicsAPI::D3D12: return "D3D12";
    case nvrhi::GraphicsAPI::VULKAN: return "Vulkan";
    }
    return "Unknown";
}

void InitializeMainState()
{
    auto &state = g_MainState;
    state.OceanWindSimulationParameters.time_scale = 1.0f;
    state.OceanWindSimulationParameters.lateral_multiplier = 1.f;
    state.OceanWindSimulationParameters.uv_warping_amplitude = 0.03f;
    state.OceanWindSimulationParameters.uv_warping_frequency = 2.f;
    state.OceanWindSimulationParameters.base_wind_direction = { 1.0f, 0.0f };
    state.OceanWindSimulationParameters.base_wind_speed = 4.7f;
    state.OceanWindSimulationParameters.base_wind_distance = 0.1f;
    state.OceanWindSimulationParameters.base_wind_dependency = 1.0f;
    state.OceanWindSimulationParameters.base_spectrum_peaking = 3.3f;
    state.OceanWindSimulationParameters.base_amplitude_multiplier = 1.0f;
    state.OceanWindSimulationParameters.swell_wind_direction = { 0.0f, 1.0f };
    state.OceanWindSimulationParameters.swell_wind_speed = 1.5f;
    state.OceanWindSimulationParameters.swell_wind_distance = 520.0f;
    state.OceanWindSimulationParameters.swell_wind_dependency = 1.0f;
    state.OceanWindSimulationParameters.swell_spectrum_peaking = 10.0f;
    state.OceanWindSimulationParameters.swell_small_waves_cutoff_length = 60.0f;
    state.OceanWindSimulationParameters.swell_small_waves_cutoff_power = 1.0f;
    state.OceanWindSimulationParameters.swell_amplitude_multiplier = 1.0f;
    state.OceanWindSimulationParameters.foam_whitecaps_threshold = 0.5f;
    state.OceanWindSimulationParameters.foam_dissipation_speed = 0.6f;
    state.OceanWindSimulationParameters.foam_falloff_speed = 0.985f;
    state.OceanWindSimulationParameters.foam_generation_amount = 0.12f;
    state.OceanWindSimulationParameters.foam_generation_threshold = 0.37f;

    state.OceanWindSimulationSettings.simulation_period = 1000.0f;
    state.OceanWindSimulationSettings.simulation_api = GFSDK_WaveWorks_Simulation_API_Compute;
    state.OceanWindSimulationSettings.detail_level = GFSDK_WaveWorks_Simulation_DetailLevel_Extreme;
    state.OceanWindSimulationSettings.enable_CPU_driven_displacement_calculation = true;
    state.OceanWindSimulationSettings.enable_GPU_driven_displacement_calculation = true;
    state.OceanWindSimulationSettings.num_readback_FIFO_entries = MainState::ReadbackArchiveSize;
    state.OceanWindSimulationSettings.CPU_simulation_threading_model = GFSDK_WaveWorks_Simulation_CPU_Threading_Model_Automatic;
    state.OceanWindSimulationSettings.use_Beaufort_scale = true;
    state.OceanWindSimulationSettings.num_GPUs = 1;
    state.OceanWindSimulationSettings.enable_GPU_timers = true;
    state.OceanWindSimulationSettings.enable_CPU_timers = true;

    state.OceanLocalSimulationParameters.amplitude_multiplier = 1.0f;
    state.OceanLocalSimulationParameters.lateral_multiplier = 1.0f;
    state.OceanLocalSimulationParameters.foam_whitecaps_threshold = 0.3f;
    state.OceanLocalSimulationParameters.foam_dissipation_speed = 0.3f;
    state.OceanLocalSimulationParameters.foam_falloff_speed = 0.6f;
    state.OceanLocalSimulationParameters.foam_generation_amount = 0.2f;
    state.OceanLocalSimulationParameters.foam_generation_threshold = 0.6f;
    state.OceanLocalSimulationSettings.simulation_domain_center = { 0, 0 };
    state.OceanLocalSimulationSettings.simulation_api = GFSDK_WaveWorks_Simulation_API_Compute;
    state.OceanLocalSimulationSettings.simulation_domain_worldspace_size = 200.0f;
    state.OceanLocalSimulationSettings.simulation_domain_grid_size = 512;
    state.OceanLocalSimulationSettings.enable_CPU_driven_displacement_calculation = true;
    state.OceanLocalSimulationSettings.enable_GPU_driven_displacement_calculation = true;
    state.OceanLocalSimulationSettings.CPU_simulation_threading_model = GFSDK_WaveWorks_Simulation_CPU_Threading_Model_Automatic;
    state.OceanLocalSimulationSettings.num_GPUs = 1;
    state.OceanLocalSimulationSettings.enable_GPU_timers = true;
    state.OceanLocalSimulationSettings.enable_CPU_timers = true;

    state.OceanQuadtreeParameters.min_patch_length = 5.0f;
    state.OceanQuadtreeParameters.max_edge_length = 10.0f;
    state.OceanQuadtreeParameters.cell_count = 64;
    state.OceanQuadtreeParameters.mean_sea_level = 0.0f;
    state.OceanQuadtreeParameters.max_LOD_number = 15;
    state.OceanQuadtreeParameters.geomorphing_degree = 1.0f;
    state.OceanQuadtreeParameters.generate_diamond_pattern = false;
    state.bForceKick = true;
}

bool CreateSimulations(nvrhi::GraphicsAPI backend)
{
    auto *device = g_DeviceManager->GetDevice();
    auto &state = g_MainState;
    gfsdk_wwresult windResult = gfsdk_wwresult_FAIL;
    gfsdk_wwresult localResult = gfsdk_wwresult_FAIL;

    if (backend == nvrhi::GraphicsAPI::D3D11)
    {
        void *context = device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);
        windResult = GFSDK_WaveWorks_Wind_Waves_Simulation_CreateDirectX11(context, true,
            state.OceanWindSimulationSettings, state.OceanWindSimulationParameters, &state.hOceanWindSimulation);
        localResult = GFSDK_WaveWorks_Local_Waves_Simulation_CreateDirectX11(context, true,
            state.OceanLocalSimulationSettings, state.OceanLocalSimulationParameters, &state.hOceanLocalSimulation);
    }
    else if (backend == nvrhi::GraphicsAPI::D3D12)
    {
        void *nativeDevice = device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);
        void *queue = device->getNativeQueue(nvrhi::ObjectTypes::D3D12_CommandQueue, nvrhi::CommandQueue::Graphics);
        windResult = GFSDK_WaveWorks_Wind_Waves_Simulation_CreateDirectX12(nativeDevice, queue, true,
            state.OceanWindSimulationSettings, state.OceanWindSimulationParameters, &state.hOceanWindSimulation);
        localResult = GFSDK_WaveWorks_Local_Waves_Simulation_CreateDirectX12(nativeDevice, queue, true,
            state.OceanLocalSimulationSettings, state.OceanLocalSimulationParameters, &state.hOceanLocalSimulation);
    }
    else
    {
        auto instance = static_cast<VkInstance>(device->getNativeObject(nvrhi::ObjectTypes::VK_Instance));
        auto physicalDevice = static_cast<VkPhysicalDevice>(device->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice));
        auto nativeDevice = static_cast<VkDevice>(device->getNativeObject(nvrhi::ObjectTypes::VK_Device));
        auto queue = static_cast<VkQueue>(device->getNativeQueue(nvrhi::ObjectTypes::VK_Queue, nvrhi::CommandQueue::Graphics));
        auto loader = GetModuleHandleW(L"vulkan-1.dll");
        if (!loader) return false;
        auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
        if (!getProc) return false;
        auto getFamilies = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            getProc(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        if (!getFamilies) return false;
        uint32_t familyCount = 0;
        getFamilies(physicalDevice, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        getFamilies(physicalDevice, &familyCount, families.data());
        int graphicsFamily = -1;
        // Donut selects the first graphics family. Use its queue for all SDK
        // submissions so simulation and rendering remain ordered.
        for (uint32_t i = 0; i < familyCount; ++i) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                graphicsFamily = int(i);
                break;
            }
        }
        if (graphicsFamily < 0) return false;
        std::vector<std::string> instanceExtensions, deviceExtensions, layers;
        g_DeviceManager->GetEnabledVulkanInstanceExtensions(instanceExtensions);
        g_DeviceManager->GetEnabledVulkanDeviceExtensions(deviceExtensions);
        g_DeviceManager->GetEnabledVulkanLayers(layers);
        std::vector<const char*> instanceNames, deviceNames, layerNames;
        for (const auto& name : instanceExtensions) instanceNames.push_back(name.c_str());
        for (const auto& name : deviceExtensions) deviceNames.push_back(name.c_str());
        for (const auto& name : layers) layerNames.push_back(name.c_str());
        windResult = GFSDK_WaveWorks_Wind_Waves_Simulation_CreateVK(
            &instance, &physicalDevice, &nativeDevice,
            &queue, graphicsFamily, &queue, graphicsFamily,
            &queue, graphicsFamily,
            instanceNames.data(), static_cast<int32_t>(instanceNames.size()),
            layerNames.data(), static_cast<int32_t>(layerNames.size()),
            deviceNames.data(), static_cast<int32_t>(deviceNames.size()), true,
            state.OceanWindSimulationSettings, state.OceanWindSimulationParameters, &state.hOceanWindSimulation);
        localResult = GFSDK_WaveWorks_Local_Waves_Simulation_CreateVK(
            &instance, &physicalDevice, &nativeDevice,
            &queue, graphicsFamily, &queue, graphicsFamily,
            &queue, graphicsFamily,
            instanceNames.data(), static_cast<int32_t>(instanceNames.size()),
            layerNames.data(), static_cast<int32_t>(layerNames.size()),
            deviceNames.data(), static_cast<int32_t>(deviceNames.size()), true,
            state.OceanLocalSimulationSettings, state.OceanLocalSimulationParameters, &state.hOceanLocalSimulation);
    }

    if (windResult != gfsdk_wwresult_OK || localResult != gfsdk_wwresult_OK)
        return false;
    return GFSDK_WaveWorks_Quadtree_Create(state.OceanQuadtreeParameters, &state.hOceanQuadtree) == gfsdk_wwresult_OK;
}

void Cleanup()
{
    if (g_DeviceManager && g_DeviceManager->GetDevice())
        g_DeviceManager->GetDevice()->waitForIdle();
    if (g_DeviceManager)
    {
        if (g_ScenePipeline)
            g_DeviceManager->RemoveRenderPass(g_ScenePipeline);
        else
        {
            if (g_UIRenderer) g_DeviceManager->RemoveRenderPass(g_UIRenderer);
            if (g_SkyRenderer) g_DeviceManager->RemoveRenderPass(g_SkyRenderer);
            if (g_OceanRenderer) g_DeviceManager->RemoveRenderPass(g_OceanRenderer);
        }
    }
    if (g_ScenePipeline) g_ScenePipeline->Shutdown();
    g_ScenePipeline = nullptr;
    g_UIRenderer = nullptr;
    if (g_SkyRenderer) g_SkyRenderer->Shutdown();
    g_SkyRenderer = nullptr;
    if (g_OceanRenderer) g_OceanRenderer->Shutdown();
    g_OceanRenderer = nullptr;

    if (g_MainState.hOceanWindSimulation)
        GFSDK_WaveWorks_Wind_Waves_Simulation_Destroy(g_MainState.hOceanWindSimulation);
    if (g_MainState.hOceanLocalSimulation)
        GFSDK_WaveWorks_Local_Waves_Simulation_Destroy(g_MainState.hOceanLocalSimulation);
    if (g_MainState.hOceanQuadtree)
        GFSDK_WaveWorks_Quadtree_Destroy(g_MainState.hOceanQuadtree);
    g_MainState.hOceanWindSimulation = nullptr;
    g_MainState.hOceanLocalSimulation = nullptr;
    g_MainState.hOceanQuadtree = nullptr;

    if (g_WaveWorksInitialized)
    {
        GFSDK_WaveWorks_Release();
        g_WaveWorksInitialized = false;
    }
    if (g_DeviceManager)
    {
        g_DeviceManager->Shutdown();
        g_DeviceManager = nullptr;
    }
}
}

int RunExample(int argc, char **argv, const ExampleSceneDefinition& scene)
{
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);

    ExampleOptions options;
    if (ParseExampleOptions(argc, argv, options) != 0)
        return 2;
    if (options.beachTerrain && !scene.supportsBeachTerrain)
    {
        std::fprintf(stderr, "error: --beach-terrain is only supported by waveworks_beach\n");
        return 2;
    }

    donut::log::EnableOutputToConsole(true);
    donut::log::EnableOutputToMessageBox(false);
    std::string error;
    g_DeviceManager = nvrhi::TakeOver(donut::app::DeviceManager::Create(options.backend));
    if (!g_DeviceManager) {
        std::fprintf(stderr, "error: Cannot create Donut device manager\n");
        return 1;
    }

    std::fprintf(stderr, "WaveWorks: initializing SDK\n");
    if (GFSDK_WaveWorks_Init(nullptr, GFSDK_WAVEWORKS_API_GUID) != gfsdk_wwresult_OK)
    {
        std::fprintf(stderr, "error: WaveWorks initialization failed\n");
        Cleanup();
        return 1;
    }
    g_WaveWorksInitialized = true;
    InitializeMainState();
    g_MainState.bUpdateWindWaves = options.updateWindWaves;
    g_MainState.bUpdateLocalWaves = options.updateLocalWaves;
    g_MainState.bBeachCoveTerrain = options.beachTerrain == BeachTerrainMode::Cove;
    if (scene.configure)
        scene.configure(g_MainState);
    ApplySimulationConfiguration(
        options,
        g_MainState.OceanWindSimulationSettings,
        g_MainState.OceanLocalSimulationSettings,
        g_MainState.iReadbackUsage);

    Microsoft::WRL::ComPtr<IDXGIAdapter> selectedAdapter;
    if (options.simulationDevice &&
        options.simulationDevice->mode == SimulationDeviceMode::Gpu &&
        options.simulationDevice->adapterIndex)
    {
        if (options.backend == nvrhi::GraphicsAPI::VULKAN)
        {
            std::fprintf(stderr, "error: indexed GPU adapter selection is not supported for Vulkan\n");
            Cleanup();
            return 1;
        }
        if (!ResolveDxgiAdapter(*options.simulationDevice->adapterIndex, selectedAdapter, error))
        {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            Cleanup();
            return 1;
        }
    }

    donut::app::DeviceCreationParameters deviceParams;
    deviceParams.enableDebugRuntime =
        ReadProcessEnvironmentVariable("WAVEWORKS_VALIDATE_RHI") == "1";
    deviceParams.enableNvrhiValidationLayer = deviceParams.enableDebugRuntime;
    deviceParams.backBufferWidth = 1280;
    deviceParams.backBufferHeight = scene.backBufferHeight;
    deviceParams.swapChainFormat = options.backend == nvrhi::GraphicsAPI::VULKAN
        ? nvrhi::Format::BGRA8_UNORM : nvrhi::Format::RGBA8_UNORM;
    // Win32 Vulkan surfaces commonly require at least three images. The
    // Vulkan device manager rejects a device when this request is
    // below VkSurfaceCapabilitiesKHR::minImageCount.
    deviceParams.swapChainBufferCount = options.backend == nvrhi::GraphicsAPI::VULKAN ? 3 : 2;
    if (selectedAdapter)
    {
        // TODO(waveworks): allow rendering and WaveWorks simulation to select different adapters.
        if (!g_DeviceManager->CreateInstance(deviceParams)) {
            Cleanup();
            return 1;
        }
        std::vector<donut::app::AdapterInfo> adapters;
        DXGI_ADAPTER_DESC requested{};
        if (FAILED(selectedAdapter->GetDesc(&requested)) ||
            !g_DeviceManager->EnumerateAdapters(adapters)) {
            Cleanup();
            return 1;
        }
        for (size_t i = 0; i < adapters.size(); ++i) {
            if (adapters[i].luid && std::memcmp(adapters[i].luid->data(),
                    &requested.AdapterLuid, sizeof(LUID)) == 0) {
                deviceParams.adapterIndex = int(i);
                break;
            }
        }
        if (deviceParams.adapterIndex < 0) {
            std::fprintf(stderr, "error: Selected adapter is not available to Donut\n");
            Cleanup();
            return 1;
        }
    }
    if (options.backend == nvrhi::GraphicsAPI::VULKAN) {
        deviceParams.deviceCreateInfoCallback = [](VkDeviceCreateInfo& desc) {
            const_cast<VkPhysicalDeviceFeatures*>(desc.pEnabledFeatures)->shaderFloat64 = VK_TRUE;
        };
    }
    const std::string windowTitle = std::string("WaveWorks Demo (") + BackendName(options.backend) + ")";
    std::fprintf(stderr, "WaveWorks: creating %s window and device\n", BackendName(options.backend));
    if (!g_DeviceManager->CreateWindowDeviceAndSwapChain(deviceParams, windowTitle.c_str()))
    {
        std::fprintf(stderr, "error: Cannot initialize the selected %s device\n", BackendName(options.backend));
        Cleanup();
        return 1;
    }

    std::fprintf(stderr, "WaveWorks: creating simulations\n");
    if (!CreateSimulations(options.backend))
    {
        std::fprintf(stderr, "error: Cannot create WaveWorks simulations\n");
        Cleanup();
        return 1;
    }


    std::fprintf(stderr, "WaveWorks: creating render passes\n");
    g_UIRenderer = MAKE_RC_OBJ_PTR(UIRenderer, g_DeviceManager.Get(), &g_MainState);
    g_SkyRenderer = MAKE_RC_OBJ_PTR(SkyRenderer, g_DeviceManager.Get(), &g_MainState);
    g_OceanRenderer = MAKE_RC_OBJ_PTR(OceanRenderer, g_DeviceManager.Get(), &g_MainState);
    if (scene.createPipeline)
        g_ScenePipeline = scene.createPipeline(g_DeviceManager, &g_MainState,
            g_SkyRenderer, g_OceanRenderer, g_UIRenderer);

    // Beach registers one orchestrator so frame order and frame-resource
    // ownership cannot depend on DeviceManager front/back insertion semantics.
    if (g_ScenePipeline)
        g_DeviceManager->AddRenderPassToBack(g_ScenePipeline);
    else {
        g_DeviceManager->AddRenderPassToFront(g_OceanRenderer);
        g_DeviceManager->AddRenderPassToBack(g_SkyRenderer);
        g_DeviceManager->AddRenderPassToBack(g_UIRenderer);
    }
    const bool passesInitialized = g_ScenePipeline
        ? g_ScenePipeline->Init(deviceParams.backBufferWidth,
            deviceParams.backBufferHeight, deviceParams.swapChainSampleCount)
        : g_UIRenderer->Init() && g_SkyRenderer->Init() && g_OceanRenderer->Init();
    if (!passesInitialized)
    {
        std::fprintf(stderr, "error: Cannot initialize render passes\n");
        Cleanup();
        return 1;
    }
    g_OceanRenderer->ResetCameraView();

    std::fprintf(stderr, "WaveWorks: rendering\n");
    g_DeviceManager->RunMessageLoop();
    Cleanup();
    return 0;
}

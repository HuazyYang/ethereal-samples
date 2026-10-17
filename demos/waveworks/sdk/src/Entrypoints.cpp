#include "CustomMemory.h"
#include "Local_Waves_Simulation.h"
#include "Quadtree.h"
#include "Wind_Waves_Simulation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <new>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace {
constexpr float kCascadeScale = 5.23f;
constexpr float kCascadeGridRadius = 45.2512016f;

constexpr float kBeaufortAmplitude[] = {
    0.1f, 1.4f, 1.4f, 1.4f, 1.4f, 1.4f, 1.4f, 1.4f, 1.4f, 1.4f, 1.4f, 1.4f, 1.4f};
constexpr float kBeaufortWindSpeed[] = {
    0.1f, 0.8f, 2.4f, 4.3f, 6.7f, 9.3f, 12.3f, 15.5f, 18.9f, 22.6f, 26.4f, 30.5f, 40.0f};
constexpr float kBeaufortWindDistance[] = {
    10000.0f, 15000.0f, 20000.0f, 25000.0f, 30000.0f, 55000.0f, 75000.0f,
    85000.0f, 100000.0f, 120000.0f, 150000.0f, 180000.0f, 250000.0f};
constexpr float kBeaufortSpectrumPeaking[] = {
    1.0f, 3.3f, 3.3f, 3.3f, 3.3f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f};
constexpr float kBeaufortChoppiness[] = {
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
constexpr float kBeaufortWhitecaps[] = {
    0.5f, 0.5f, 0.4f, 0.35f, 0.38f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
constexpr float kBeaufortGenerationThreshold[] = {
    0.1f, 0.1f, 0.1f, 0.05f, 0.15f, 0.23f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
constexpr float kBeaufortGenerationAmount[] = {
    0.0f, 0.0f, 0.0f, 0.12f, 0.12f, 0.12f, 0.15f, 0.15f, 0.15f, 0.15f, 0.15f, 0.15f, 0.15f};
constexpr float kBeaufortDissipation[] = {
    0.0f, 0.0f, 0.0f, 0.08f, 0.1f, 0.4f, 0.4f, 0.4f, 0.4f, 0.4f, 0.5f, 0.6f, 0.6f};
constexpr float kBeaufortFalloff[] = {
    0.985f, 0.985f, 0.985f, 0.98f, 0.985f, 0.985f, 0.985f,
    0.988f, 0.985f, 0.99f, 0.991f, 0.992f, 0.993f};

float interpolateBeaufort(const float* values, int32_t index, float fraction) {
    return values[index] + (values[index + 1u] - values[index]) * fraction;
}

bool isExpectedApiGuid(const GFSDK_WaveWorks_API_GUID& guid) {
    const GFSDK_WaveWorks_API_GUID expected = GFSDK_WAVEWORKS_API_GUID;
    return guid.Component1 == expected.Component1 &&
           guid.Component2 == expected.Component2 &&
           guid.Component3 == expected.Component3 && guid.Component4 == expected.Component4;
}

uint32_t makeLocalTextureSize(uint32_t requestedSize) {
    uint32_t textureSize = 1;
    while (textureSize < requestedSize)
        textureSize <<= 1u;
    if (textureSize < 64u)
        textureSize = 64u;
    if (textureSize > 2048u)
        textureSize = 2048u;
    return textureSize;
}

Local_Waves_Detailed_Simulation_Parameters makeLocalDetailedParams(
    const GFSDK_WaveWorks_Local_Waves_Simulation_Settings& settings,
    const GFSDK_WaveWorks_Local_Waves_Simulation_Parameters& params) {
    Local_Waves_Detailed_Simulation_Parameters detailed{};
    detailed.amplitudeMultiplier = params.amplitude_multiplier;
    detailed.lateralMultiplier = params.lateral_multiplier;
    detailed.foamWhitecapsThreshold = params.foam_whitecaps_threshold;
    detailed.foamGenerationThreshold = params.foam_generation_threshold;
    detailed.foamGenerationAmount = params.foam_generation_amount;
    detailed.foamDissipationSpeed = params.foam_dissipation_speed;
    detailed.foamFalloffSpeed = params.foam_falloff_speed;
    detailed.textureSize = makeLocalTextureSize(settings.simulation_domain_grid_size);
    detailed.offsetX = settings.simulation_domain_center.x;
    detailed.offsetY = settings.simulation_domain_center.y;
    detailed.physicalSize = settings.simulation_domain_worldspace_size;
    detailed.simulationAPI = settings.simulation_api;
    detailed.enableCpuDrivenDisplacementCalculation =
        settings.enable_CPU_driven_displacement_calculation;
    detailed.enableGpuDrivenDisplacementCalculation =
        settings.enable_GPU_driven_displacement_calculation;
    detailed.cpuThreadingModel = settings.CPU_simulation_threading_model;
    detailed.enableGpuTimers = settings.enable_GPU_timers;
    detailed.enableCpuTimers = settings.enable_CPU_timers;
    return detailed;
}

uint32_t textureSizeFromDetailLevel(GFSDK_WaveWorks_Simulation_DetailLevel detailLevel) {
    switch (detailLevel) {
        case GFSDK_WaveWorks_Simulation_DetailLevel_Normal:
            return 128u;
        case GFSDK_WaveWorks_Simulation_DetailLevel_High:
            return 256u;
        case GFSDK_WaveWorks_Simulation_DetailLevel_Extreme:
        default:
            return 512u;
    }
}

Wind_Waves_Detailed_Simulation_Parameters makeWindDetailedParams(
    const GFSDK_WaveWorks_Wind_Waves_Simulation_Settings& settings,
    const GFSDK_WaveWorks_Wind_Waves_Simulation_Parameters& params) {
    Wind_Waves_Detailed_Simulation_Parameters detailed{};
    const uint32_t textureSize = textureSizeFromDetailLevel(settings.detail_level);
    const float beaufortFloor = std::floor(params.base_wind_speed);
    int32_t beaufortIndex = static_cast<int32_t>(beaufortFloor);
    float beaufortFraction = params.base_wind_speed - beaufortFloor;
    if (beaufortIndex > 11) {
        beaufortIndex = 11;
        beaufortFraction = params.base_wind_speed - 11.0f;
    }

    detailed.foamWhitecapsThreshold = settings.use_Beaufort_scale
        ? interpolateBeaufort(kBeaufortWhitecaps, beaufortIndex, beaufortFraction)
        : params.foam_whitecaps_threshold;
    detailed.foamWhitecapsThreshold /=
        (std::log2f(settings.simulation_period / 1000.0f + 1.0f) + 1.0f) * 0.5f;
    detailed.timeScale = params.time_scale;
    detailed.numGPUs = std::max(1u, std::min(settings.num_GPUs, 4u));
    detailed.simulationAPI = settings.simulation_api;
    detailed.cpuThreadingModel = settings.CPU_simulation_threading_model;
    detailed.enableGpuTimers = settings.enable_GPU_timers;
    detailed.enableCpuTimers = settings.enable_CPU_timers;
    detailed.enableGpuDrivenDisplacementCalculation =
        settings.enable_GPU_driven_displacement_calculation;

    for (uint32_t i = 0; i < 4u; ++i) {
        Wind_Waves_Detailed_Simulation_Parameters::Cascade& cascade = detailed.cascades[i];
        cascade.numGPUs = settings.num_GPUs;
        cascade.textureSize = i < 3u ? std::min(textureSize, 256u) : textureSize;
        cascade.physicalSize =
            settings.simulation_period / powf(kCascadeScale, static_cast<float>(i));
        cascade.displacementScale = params.lateral_multiplier;
        cascade.baseWindDirection = params.base_wind_direction;
        cascade.baseWindSpeed = params.base_wind_speed;
        cascade.baseWindDistance = params.base_wind_distance * 1000.0f;
        cascade.baseSpectrumPeaking = params.base_spectrum_peaking;
        cascade.baseWindDependency = params.base_wind_dependency;
        cascade.baseSmallWavesCutoffLength = params.base_small_waves_cutoff_length;
        cascade.baseSmallWavesCutoffPower = params.base_small_waves_cutoff_power;
        cascade.baseAmplitudeMultiplier = params.base_amplitude_multiplier;
        cascade.swellWindDirection = params.swell_wind_direction;
        cascade.swellWindSpeed = params.swell_wind_speed;
        cascade.swellWindDistance = params.swell_wind_distance * 1000.0f;
        cascade.swellWindDependency = params.swell_wind_dependency;
        cascade.swellSpectrumPeaking = params.swell_spectrum_peaking;
        cascade.swellSmallWavesCutoffLength = params.swell_small_waves_cutoff_length;
        cascade.swellSmallWavesCutoffPower = params.swell_small_waves_cutoff_power;
        cascade.swellAmplitudeMultiplier = params.swell_amplitude_multiplier;
        cascade.writeDisplacementMap = settings.enable_CPU_driven_displacement_calculation;
        cascade.archiveSlots = settings.num_readback_FIFO_entries;
        cascade.upperGridRadius = i < 3u ? kCascadeGridRadius : static_cast<float>(cascade.textureSize);
        cascade.lowerGridRadius = i == 0u ? 0.0f
            : cascade.physicalSize * detailed.cascades[i - 1u].upperGridRadius /
                detailed.cascades[i - 1u].physicalSize;
        cascade.warpAmplitude = params.uv_warping_amplitude;
        cascade.wrapFrequency = params.uv_warping_frequency;
        cascade.foamGenerationThreshold = params.foam_generation_threshold;
        cascade.foamGenerationAmount = params.foam_generation_amount;
        cascade.foamDissipationSpeed = params.foam_dissipation_speed;
        cascade.foamFalloffSpeed = params.foam_falloff_speed;

        if (settings.use_Beaufort_scale) {
            cascade.displacementScale = interpolateBeaufort(
                kBeaufortChoppiness, beaufortIndex, beaufortFraction);
            cascade.baseAmplitudeMultiplier = interpolateBeaufort(
                kBeaufortAmplitude, beaufortIndex, beaufortFraction);
            cascade.baseWindSpeed = interpolateBeaufort(
                kBeaufortWindSpeed, beaufortIndex, beaufortFraction);
            cascade.baseWindDistance = interpolateBeaufort(
                kBeaufortWindDistance, beaufortIndex, beaufortFraction);
            cascade.baseSpectrumPeaking = interpolateBeaufort(
                kBeaufortSpectrumPeaking, beaufortIndex, beaufortFraction);
            cascade.foamGenerationThreshold = interpolateBeaufort(
                kBeaufortGenerationThreshold, beaufortIndex, beaufortFraction);
            cascade.foamGenerationAmount = interpolateBeaufort(
                kBeaufortGenerationAmount, beaufortIndex, beaufortFraction);
            cascade.foamDissipationSpeed = interpolateBeaufort(
                kBeaufortDissipation, beaufortIndex, beaufortFraction);
            cascade.foamFalloffSpeed = interpolateBeaufort(
                kBeaufortFalloff, beaufortIndex, beaufortFraction);
        }
    }

    return detailed;
}

}  // namespace

GFSDK_WAVEWORKS_DECL(const char*) GFSDK_WaveWorks_GetBuildString() {
    return "WINDOWS_TEST";
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Init(const GFSDK_WaveWorks_Malloc_Hooks* pOptionalMallocHooks,
                     const GFSDK_WaveWorks_API_GUID& apiGUID) {
    if (!isExpectedApiGuid(apiGUID)) {
        diagnostic_message(
            L"ERROR: GFSDK_WaveWorks_Init was called with an invalid API GUID\n");
        return WWE_FAIL;
    }

    if (!pOptionalMallocHooks)
        return WWE_OK;

    if (!pOptionalMallocHooks->pMalloc || !pOptionalMallocHooks->pFree ||
        !pOptionalMallocHooks->pAlignedMalloc || !pOptionalMallocHooks->pAlignedFree) {
        diagnostic_message(
            L"SetMemoryManagementCallbacks received invalid pointer to memory allocation "
            L"routines\n");
        return WWE_FAIL;
    }

    diagnostic_message(
        L"SetMemoryManagementCallbacks cannot assign custom allocation routines with "
        L"static builds of the WaveWorks library\n");
    return WWE_FAIL;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult) GFSDK_WaveWorks_Release() {
    resetMemoryManagementCallbacksToDefaults();
    return WWE_OK;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Local_Waves_Simulation_CreateDirectX11(
    void* pDeviceContext, bool enable_graphics,
    const GFSDK_WaveWorks_Local_Waves_Simulation_Settings& settings,
    const GFSDK_WaveWorks_Local_Waves_Simulation_Parameters& params,
    GFSDK_WaveWorks_Local_Waves_SimulationHandle* pResult) {
    Local_Waves_Simulation* simulation = new (std::nothrow) Local_Waves_Simulation();

    const Local_Waves_Detailed_Simulation_Parameters detailed =
        makeLocalDetailedParams(settings, params);
    const WWRESULT result = simulation->init11(pDeviceContext, detailed, enable_graphics);
    if (result != WWE_OK) {
        delete simulation;
        return WWE_FAIL;
    }
    *pResult = simulation;
    return WWE_OK;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Local_Waves_Simulation_CreateDirectX12(
    void* pDevice, void* pQueue, bool enable_graphics,
    const GFSDK_WaveWorks_Local_Waves_Simulation_Settings& settings,
    const GFSDK_WaveWorks_Local_Waves_Simulation_Parameters& params,
    GFSDK_WaveWorks_Local_Waves_SimulationHandle* pResult) {
    Local_Waves_Simulation* simulation = new (std::nothrow) Local_Waves_Simulation();

    const Local_Waves_Detailed_Simulation_Parameters detailed =
        makeLocalDetailedParams(settings, params);
    const WWRESULT result = simulation->init12(pDevice, pQueue, detailed, enable_graphics);
    if (result != WWE_OK) {
        delete simulation;
        return WWE_FAIL;
    }
    *pResult = simulation;
    return WWE_OK;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Local_Waves_Simulation_CreateVK(
    void* pVkInstance, void* pVulkanPhysicalDevice, void* pVulkanDevice,
    void* pGraphicsQueue, int32_t graphicsQueueIndex, void* pTransferQueue,
    int32_t transferQueueIndex, void* pComputeQueue, int32_t computeQueueIndex,
    const char** instanceExtensions, int32_t numInstanceExtensions, const char** layers,
    int32_t numLayers, const char** deviceExtensions, int32_t numDeviceExtensions,
    bool enable_graphics, const GFSDK_WaveWorks_Local_Waves_Simulation_Settings& settings,
    const GFSDK_WaveWorks_Local_Waves_Simulation_Parameters& params,
    GFSDK_WaveWorks_Local_Waves_SimulationHandle* pResult) {
    Local_Waves_Simulation* simulation = new (std::nothrow) Local_Waves_Simulation();

    const Local_Waves_Detailed_Simulation_Parameters detailed =
        makeLocalDetailedParams(settings, params);
    const WWRESULT result = simulation->initVK(
        pVkInstance, pVulkanPhysicalDevice, pVulkanDevice, pGraphicsQueue,
        graphicsQueueIndex, pTransferQueue, transferQueueIndex, pComputeQueue,
        computeQueueIndex, instanceExtensions, numInstanceExtensions, layers, numLayers,
        deviceExtensions, numDeviceExtensions, detailed, enable_graphics);
    if (result != WWE_OK) {
        delete simulation;
        return WWE_FAIL;
    }
    *pResult = simulation;
    return WWE_OK;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Local_Waves_Simulation_Destroy(
    GFSDK_WaveWorks_Local_Waves_SimulationHandle hSim) {
    delete hSim;
    return WWE_OK;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Local_Waves_Simulation_UpdateProperties(
    GFSDK_WaveWorks_Local_Waves_SimulationHandle hSim,
    const GFSDK_WaveWorks_Local_Waves_Simulation_Settings& settings,
    const GFSDK_WaveWorks_Local_Waves_Simulation_Parameters& params) {
    const Local_Waves_Detailed_Simulation_Parameters detailed =
        makeLocalDetailedParams(settings, params);
    return hSim->reinit(detailed);
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Local_Waves_Simulation_SetDeltaTime(
    GFSDK_WaveWorks_Local_Waves_SimulationHandle hSim, float fDeltaTime) {
    hSim->setSimulationDeltaTime(fDeltaTime);
    return WWE_OK;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Local_Waves_GetDataForRendering(
    GFSDK_WaveWorks_Local_Waves_SimulationHandle hSim,
    GFSDK_WaveWorks_Local_Waves_Rendering_Data& renderingData) {
    return hSim->getRenderingResources(renderingData);
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Local_Waves_Simulation_GetDisplacements(
    GFSDK_WaveWorks_Local_Waves_SimulationHandle hSim, const gfsdk_float2* pInSamplePoints,
    gfsdk_float4* pOutDisplacements, uint32_t numSamples, bool calculateOnGPU) {
    return hSim->getDisplacements(pInSamplePoints, pOutDisplacements, numSamples,
                                  calculateOnGPU);
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Local_Waves_Simulation_GetStats(
    GFSDK_WaveWorks_Local_Waves_SimulationHandle hSim,
    GFSDK_WaveWorks_Local_Waves_Simulation_Stats& stats) {
    hSim->getStats(stats);
    return WWE_OK;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Local_Waves_Simulation_Kick(
    GFSDK_WaveWorks_Local_Waves_SimulationHandle hSim, uint64_t* pOutKickID) {
    return hSim->kick(pOutKickID);
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Local_Waves_Simulation_GetStagingCursor(
    GFSDK_WaveWorks_Local_Waves_SimulationHandle hSim, uint64_t* pOutKickID) {
    return hSim->getStagingCursor(pOutKickID) ? WWE_OK : WWE_NONE;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Local_Waves_Simulation_AdvanceStagingCursor(
    GFSDK_WaveWorks_Local_Waves_SimulationHandle hSim, bool block) {
    bool wouldBlock = false;
    const WWRESULT result = hSim->advanceStagingCursor(block, wouldBlock);
    if (result == WWE_OK)
        return WWE_OK;
    if (result == WWE_NONE)
        return wouldBlock ? WWE_WOULD_BLOCK : WWE_NONE;
    return WWE_INTERNAL_ERROR;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Local_Waves_Simulation_WaitStagingCursor(
    GFSDK_WaveWorks_Local_Waves_SimulationHandle hSim) {
    return hSim->waitStagingCursor();
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Local_Waves_Simulation_GetReadbackCursor(
    GFSDK_WaveWorks_Local_Waves_SimulationHandle hSim, uint64_t* pOutKickID) {
    return hSim->getReadbackCursor(pOutKickID) ? WWE_OK : WWE_NONE;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Local_Waves_Simulation_AdvanceReadbackCursor(
    GFSDK_WaveWorks_Local_Waves_SimulationHandle hSim, bool block) {
    bool wouldBlock = false;
    const WWRESULT result = hSim->advanceReadbackCursor(block, wouldBlock);
    WWS_UNUSED(wouldBlock);
    return result;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Local_Waves_Simulation_Reset(
    GFSDK_WaveWorks_Local_Waves_SimulationHandle hSim) {
    hSim->resetSimulation();
    return WWE_OK;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Local_Waves_Simulation_AddDisturbances(
    GFSDK_WaveWorks_Local_Waves_SimulationHandle hSim, gfsdk_float4* pInDisturbanceData,
    uint32_t numDisturbanceSamples) {
    hSim->addDisturbances(pInDisturbanceData, numDisturbanceSamples);
    return WWE_OK;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Wind_Waves_Simulation_CreateDirectX11(
    void* pDeviceContext, bool enable_graphics,
    const GFSDK_WaveWorks_Wind_Waves_Simulation_Settings& settings,
    const GFSDK_WaveWorks_Wind_Waves_Simulation_Parameters& params,
    GFSDK_WaveWorks_Wind_Waves_SimulationHandle* pResult) {
    Wind_Waves_Simulation* simulation = new (std::nothrow) Wind_Waves_Simulation();

    const Wind_Waves_Detailed_Simulation_Parameters detailed =
        makeWindDetailedParams(settings, params);
    const WWRESULT result = simulation->init11(pDeviceContext, detailed, enable_graphics);
    if (result != WWE_OK) {
        delete simulation;
        return WWE_FAIL;
    }
    *pResult = simulation;
    return WWE_OK;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Wind_Waves_Simulation_CreateDirectX12(
    void* pDevice, void* pQueue, bool enable_graphics,
    const GFSDK_WaveWorks_Wind_Waves_Simulation_Settings& settings,
    const GFSDK_WaveWorks_Wind_Waves_Simulation_Parameters& params,
    GFSDK_WaveWorks_Wind_Waves_SimulationHandle* pResult) {
    Wind_Waves_Simulation* simulation = new (std::nothrow) Wind_Waves_Simulation();

    const Wind_Waves_Detailed_Simulation_Parameters detailed =
        makeWindDetailedParams(settings, params);
    const WWRESULT result = simulation->init12(pDevice, pQueue, detailed, enable_graphics);
    if (result != WWE_OK) {
        delete simulation;
        return WWE_FAIL;
    }
    *pResult = simulation;
    return WWE_OK;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Wind_Waves_Simulation_CreateVK(
    void* pVkInstance, void* pVulkanPhysicalDevice, void* pVulkanDevice,
    void* pGraphicsQueue, int32_t graphicsQueueIndex, void* pTransferQueue,
    int32_t transferQueueIndex, void* pComputeQueue, int32_t computeQueueIndex,
    const char** instanceExtensions, int32_t numInstanceExtensions, const char** layers,
    int32_t numLayers, const char** deviceExtensions, int32_t numDeviceExtensions,
    bool enable_graphics, const GFSDK_WaveWorks_Wind_Waves_Simulation_Settings& settings,
    const GFSDK_WaveWorks_Wind_Waves_Simulation_Parameters& params,
    GFSDK_WaveWorks_Wind_Waves_SimulationHandle* pResult) {
    Wind_Waves_Simulation* simulation = new (std::nothrow) Wind_Waves_Simulation();

    const Wind_Waves_Detailed_Simulation_Parameters detailed =
        makeWindDetailedParams(settings, params);
    const WWRESULT result = simulation->initVK(
        pVkInstance, pVulkanPhysicalDevice, pVulkanDevice, pGraphicsQueue,
        graphicsQueueIndex, pTransferQueue, transferQueueIndex, pComputeQueue,
        computeQueueIndex, instanceExtensions, numInstanceExtensions, layers, numLayers,
        deviceExtensions, numDeviceExtensions, detailed, enable_graphics);
    if (result != WWE_OK) {
        delete simulation;
        return WWE_FAIL;
    }
    *pResult = simulation;
    return WWE_OK;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Wind_Waves_Simulation_Destroy(
    GFSDK_WaveWorks_Wind_Waves_SimulationHandle hSim) {
    delete hSim;
    return WWE_OK;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Wind_Waves_Simulation_UpdateProperties(
    GFSDK_WaveWorks_Wind_Waves_SimulationHandle hSim,
    const GFSDK_WaveWorks_Wind_Waves_Simulation_Settings& settings,
    const GFSDK_WaveWorks_Wind_Waves_Simulation_Parameters& params) {
    const Wind_Waves_Detailed_Simulation_Parameters detailed =
        makeWindDetailedParams(settings, params);
    return hSim->reinit(detailed);
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Wind_Waves_Simulation_SetTime(
    GFSDK_WaveWorks_Wind_Waves_SimulationHandle hSim, double dAppTime) {
    hSim->setSimulationTime(dAppTime);
    return WWE_OK;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Wind_Waves_GetDataForRendering(
    GFSDK_WaveWorks_Wind_Waves_SimulationHandle hSim,
    GFSDK_WaveWorks_Wind_Waves_Rendering_Data& renderingData) {
    return hSim->getRenderingResources(renderingData);
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Wind_Waves_Simulation_GetDisplacements(
    GFSDK_WaveWorks_Wind_Waves_SimulationHandle hSim, const gfsdk_float2* pInSamplePoints,
    gfsdk_float4* pOutDisplacements, uint32_t numSamples, bool calculateOnGPU) {
    return hSim->getDisplacements(pInSamplePoints, pOutDisplacements, numSamples,
                                  calculateOnGPU);
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Wind_Waves_Simulation_GetStats(
    GFSDK_WaveWorks_Wind_Waves_SimulationHandle hSim,
    GFSDK_WaveWorks_Wind_Waves_Simulation_Stats& stats) {
    hSim->getStats(stats);
    return WWE_OK;
}

GFSDK_WAVEWORKS_DECL(float)
GFSDK_WaveWorks_Wind_Waves_Simulation_GetConservativeMaxDisplacementEstimate(
    GFSDK_WaveWorks_Wind_Waves_SimulationHandle hSim) {
    return hSim->getConservativeMaxDisplacementEstimate();
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Wind_Waves_Simulation_Kick(GFSDK_WaveWorks_Wind_Waves_SimulationHandle hSim,
                                           uint64_t* pOutKickID) {
    return hSim->kick(pOutKickID);
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Wind_Waves_Simulation_GetStagingCursor(
    GFSDK_WaveWorks_Wind_Waves_SimulationHandle hSim, uint64_t* pOutKickID) {
    return hSim->getStagingCursor(pOutKickID) ? WWE_OK : WWE_NONE;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Wind_Waves_Simulation_AdvanceStagingCursor(
    GFSDK_WaveWorks_Wind_Waves_SimulationHandle hSim, bool block) {
    bool wouldBlock = false;
    const WWRESULT result = hSim->advanceStagingCursor(block, wouldBlock);
    if (result == WWE_OK)
        return WWE_OK;
    if (result == WWE_NONE)
        return wouldBlock ? WWE_WOULD_BLOCK : WWE_NONE;
    return WWE_INTERNAL_ERROR;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Wind_Waves_Simulation_WaitStagingCursor(
    GFSDK_WaveWorks_Wind_Waves_SimulationHandle hSim) {
    return hSim->waitStagingCursor();
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Wind_Waves_Simulation_GetReadbackCursor(
    GFSDK_WaveWorks_Wind_Waves_SimulationHandle hSim, uint64_t* pOutKickID) {
    return hSim->getReadbackCursor(pOutKickID) ? WWE_OK : WWE_NONE;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Wind_Waves_Simulation_AdvanceReadbackCursor(
    GFSDK_WaveWorks_Wind_Waves_SimulationHandle hSim, bool block) {
    bool wouldBlock = false;
    const WWRESULT result = hSim->advanceReadbackCursor(block, wouldBlock);
    static_cast<void>(wouldBlock);
    return result;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Wind_Waves_Simulation_ArchiveDisplacements(
    GFSDK_WaveWorks_Wind_Waves_SimulationHandle hSim) {
    return hSim->archiveDisplacements();
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Wind_Waves_Simulation_GetArchivedDisplacements(
    GFSDK_WaveWorks_Wind_Waves_SimulationHandle hSim, float coord,
    const gfsdk_float2* pInSamplePoints, gfsdk_float4* pOutDisplacements,
    uint32_t numSamples) {
    return hSim->getArchivedDisplacements(coord, pInSamplePoints, pOutDisplacements,
                                          numSamples);
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Quadtree_Create(const GFSDK_WaveWorks_Quadtree_Params& params,
                                GFSDK_WaveWorks_QuadtreeHandle* pResult) {
    if (!pResult)
        return WWE_FAIL;
    *pResult = nullptr;

    Quadtree* quadtree = new (std::nothrow) Quadtree(params);
    if (!quadtree)
        return WWE_FAIL;

    *pResult = quadtree;
    return WWE_OK;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Quadtree_Destroy(GFSDK_WaveWorks_QuadtreeHandle hQuadtree) {
    delete hQuadtree;
    return WWE_OK;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Quadtree_UpdateParams(GFSDK_WaveWorks_QuadtreeHandle hQuadtree,
                                      const GFSDK_WaveWorks_Quadtree_Params& params) {
    if (!hQuadtree)
        return WWE_FAIL;
    hQuadtree->reinit(params);
    return WWE_OK;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Quadtree_GetGeometryData(GFSDK_WaveWorks_QuadtreeHandle hQuadtree,
                                         uint32_t* pNumVertices,
                                         gfsdk_float2** ppVertexPositions,
                                         uint32_t* pNumIndices, uint32_t** ppIndices) {
    if (!hQuadtree || !pNumVertices || !ppVertexPositions || !pNumIndices || !ppIndices)
        return WWE_FAIL;
    return hQuadtree->getGeometryData(pNumVertices, ppVertexPositions, pNumIndices,
                                      ppIndices)
               ? WWE_OK
               : WWE_FAIL;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Quadtree_GetNodesForRendering(
    GFSDK_WaveWorks_QuadtreeHandle hQuadtree, const gfsdk_float4x4& matView,
    const gfsdk_float4x4& matProj, const gfsdk_float2& viewportSize, float cullMargin,
    uint32_t* pNumNodes, GFSDK_WaveWorks_Quadtree_NodeRenderingProperties** ppNodes) {
    if (!hQuadtree || !pNumNodes || !ppNodes)
        return WWE_FAIL;
    return hQuadtree->getNodes(matView, matProj, viewportSize, cullMargin, pNumNodes,
                               ppNodes)
               ? WWE_OK
               : WWE_FAIL;
}

GFSDK_WAVEWORKS_DECL(gfsdk_wwresult)
GFSDK_WaveWorks_Quadtree_GetStats(GFSDK_WaveWorks_QuadtreeHandle hQuadtree,
                                  GFSDK_WaveWorks_Quadtree_Stats& stats) {
    if (!hQuadtree)
        return WWE_FAIL;
    hQuadtree->getStats(stats);
    return WWE_OK;
}

void handle_b_error(const wchar_t* filename, uint32_t line) {
    diagnostic_message(L"%ls(%u): error\n", filename, line);
#if defined(_WIN32)
    DebugBreak();
#endif
}

void handle_ww_error(const wchar_t* filename, uint32_t line) {
    diagnostic_message(L"%ls(%u): error\n", filename, line);
#if defined(_WIN32)
    DebugBreak();
#endif
}

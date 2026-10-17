#ifndef SRC_WWS_WIND_WAVES_SIMULATION_H
#define SRC_WWS_WIND_WAVES_SIMULATION_H

#include <cstddef>
#include <cstdint>

#include <nvrhi/nvrhi.h>
#include <nvrhi/core/foundation.h>
#include <waveworks/GFSDK_WaveWorks.h>

#include "ThreadPool.h"
#include "Types.h"
#include "Wind_Waves_FFT_Simulation_CPU_Impl.h"
#include "Wind_Waves_FFT_Simulation_Compute_Impl.h"
#include "Wind_Waves_Simulation_Manager.h"

struct Wind_Waves_Simulation
{
    friend struct WindWavesSimulationLayoutVerifier;

    Wind_Waves_Simulation();
    ~Wind_Waves_Simulation();

    WWRESULT advanceReadbackCursor(bool block, bool& wouldBlock);
    WWRESULT advanceStagingCursor(bool block, bool& wouldBlock);
    WWRESULT allocateAll();
    WWRESULT allocateRenderingResources();
    WWRESULT allocateSimulation(
        uint32_t cascadeIndex,
        nvrhi::CommandListHandle commandList);
    WWRESULT allocateSimulationManager();
    WWRESULT archiveDisplacements();
    WWRESULT consumeAvailableTimerSlot(uint32_t& slot);
    void consumeGPUSlot();
    WWRESULT getArchivedDisplacements(
        float coord,
        const gfsdk_float2* samplePoints,
        gfsdk_float4* displacements,
        uint32_t numSamples);
    float getConservativeMaxDisplacementEstimate() const;
    WWRESULT getDisplacements(
        const gfsdk_float2* samplePoints,
        gfsdk_float4* displacements,
        uint32_t numSamples,
        bool calculateOnGPU);
    bool getReadbackCursor(uint64_t* cursor) const;
    WWRESULT getRenderingResources(GFSDK_WaveWorks_Wind_Waves_Rendering_Data& data) const;
    bool getStagingCursor(uint64_t* cursor) const;
    WWRESULT getStats(GFSDK_WaveWorks_Wind_Waves_Simulation_Stats& stats) const;
    WWRESULT init11(
        void* deviceContext,
        const Wind_Waves_Detailed_Simulation_Parameters& params,
        bool enableGraphics);
    WWRESULT init12(
        void* device,
        void* queue,
        const Wind_Waves_Detailed_Simulation_Parameters& params,
        bool enableGraphics);
    WWRESULT initVK(
        void* instance,
        void* physicalDevice,
        void* device,
        void* graphicsQueue,
        int graphicsQueueIndex,
        void* transferQueue,
        int transferQueueIndex,
        void* computeQueue,
        int computeQueueIndex,
        const char** instanceExtensions,
        int numInstanceExtensions,
        const char** layers,
        int numLayers,
        const char** deviceExtensions,
        int numDeviceExtensions,
        const Wind_Waves_Detailed_Simulation_Parameters& params,
        bool enableGraphics);
    WWRESULT kick(uint64_t* kickId);
    WWRESULT reinit(const Wind_Waves_Detailed_Simulation_Parameters& params);
    void releaseAll();
    void releaseRenderingResources();
    void releaseSimulation(uint32_t cascadeIndex);
    void releaseSimulationManager();
    void setSimulationTime(double appTime);
    bool updateGradientMaps();
    WWRESULT waitForAllInFlightTimers();
    WWRESULT waitStagingCursor();

private:
    struct MessageCallback final : public nvrhi::ObjectImpl<nvrhi::IMessageCallback>
    {
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(MessageCallback)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IMessageCallback)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_END_INTERFACE_TABLE()
        void message(
            nvrhi::MessageSeverity severity,
            const char* messageText) noexcept override;
    };

    struct CascadeState
    {
        CascadeState();
        ~CascadeState();

        Wind_Waves_FFT_Simulation* simulation; // offset 0
        uint64_t displacementMapVersion; // offset 8
        bool gpuSlotDirty[4]; // offset 16
        nvrhi::TextureHandle gradientTextures[4]; // offset 24
        nvrhi::TextureHandle momentTextures[4]; // offset 56
        nvrhi::TextureHandle foamEnergyTexture; // offset 88
        nvrhi::BindingSetHandle gradientBindingSets[4]; // offset 96
        nvrhi::BindingSetHandle momentBindingSets[4]; // offset 128
    };

    static_assert(sizeof(CascadeState) == 0xA0,
                  "Wind waves cascade state must match recovered object size.");

    Wind_Waves_Detailed_Simulation_Parameters makeDetailedParams() const;
    void allocateThreadPool(GFSDK_WaveWorks_Simulation_CPU_Threading_Model threadingModel);
    void resetTimerRing();
    void setDetailedParams(const Wind_Waves_Detailed_Simulation_Parameters& params);
    bool shouldRecreateRendering(
        const Wind_Waves_Detailed_Simulation_Parameters& params) const;

    float m_foamWhitecapsThreshold; // offset 0
    Wind_Waves_Detailed_Simulation_Parameters::Cascade m_cascades[4]; // offset 4
    float m_timeScale; // offset 516
    uint32_t m_numGPUs; // offset 520
    GFSDK_WaveWorks_Simulation_API m_simulationAPI; // offset 524
    GFSDK_WaveWorks_Simulation_CPU_Threading_Model m_cpuThreadingModel; // offset 528
    bool m_enableGpuTimers; // offset 532
    bool m_enableCpuTimers; // offset 533
    bool m_enableGpuDrivenDisplacementCalculation; // offset 534
    uint8_t m_padding0; // offset 535
    CascadeState m_cascadeStates[4]; // offset 536
    uint32_t m_numGpuSlots; // offset 1176
    uint32_t m_gpuSlotIndex; // offset 1180
    double m_simulationTime; // offset 1184
    double m_simulationTimeHistory[5]; // offset 1192
    uint32_t m_simulationTimeHistoryCount; // offset 1232
    uint32_t m_simulationTimeHistoryPadding; // offset 1236
    double m_simulationDeltaTime; // offset 1240
    Wind_Waves_Simulation_Manager* m_manager; // offset 1248
    ThreadPool* m_threadPool; // offset 1256
    nvrhi::AutoPtr<MessageCallback> m_messageCallback = MAKE_RC_OBJ_PTR(MessageCallback); // offset 1264
    nvrhi::DeviceHandle m_device; // offset 1272
    nvrhi::CommandListHandle m_commandList; // offset 1280
    bool m_enableGraphics; // offset 1288
    char m_padding1[7]; // offset 1289

    nvrhi::TextureHandle m_displacementTextureArray; // offset 1296
    nvrhi::TextureHandle m_gradientTextureArray; // offset 1304
    nvrhi::TextureHandle m_momentTextureArray; // offset 1312
    nvrhi::ShaderHandle m_gradientFoamShader; // offset 1320
    nvrhi::BufferHandle m_renderingConstantBuffer; // offset 1328
    nvrhi::ComputePipelineHandle m_gradientPipeline; // offset 1336
    nvrhi::BindingLayoutHandle m_gradientBindingLayout; // offset 1344
    nvrhi::ShaderHandle m_foamInjectionShader; // offset 1352
    nvrhi::ComputePipelineHandle m_foamInjectionPipeline; // offset 1360
    nvrhi::BindingLayoutHandle m_foamInjectionBindingLayout; // offset 1368
    nvrhi::SamplerHandle m_wrapSampler; // offset 1376
    nvrhi::ShaderHandle m_gpuSampleShader; // offset 1384
    nvrhi::BufferHandle m_readbackPositionsBuffer; // offset 1392
    nvrhi::BufferHandle m_readbackResultsBuffer; // offset 1400
    nvrhi::BufferHandle m_readbackResultsBufferStaging; // offset 1408
    nvrhi::BufferHandle m_calculateReadbacksCB; // offset 1416
    nvrhi::ComputePipelineHandle m_gpuSamplePipeline; // offset 1424
    nvrhi::BindingLayoutHandle m_gpuSampleBindingLayout; // offset 1432
    nvrhi::BindingSetHandle m_gpuSampleBindingSet; // offset 1440
    nvrhi::ShaderHandle m_textureArrayShader; // offset 1448
    nvrhi::BufferHandle m_textureArrayMipmapConstantBuffer; // offset 1456
    nvrhi::ComputePipelineHandle m_textureArrayPipeline; // offset 1464
    nvrhi::BindingSetHandle m_textureArrayBindingSets[4]; // offset 1472
    nvrhi::BindingLayoutHandle m_textureArrayBindingLayout; // offset 1504
    nvrhi::ShaderHandle m_mipmapShader; // offset 1512
    nvrhi::ComputePipelineHandle m_mipmapPipeline; // offset 1520
    nvrhi::BindingSetHandle m_displacementMipmapBindingSet0; // offset 1528
    nvrhi::BindingSetHandle m_displacementMipmapBindingSet1; // offset 1536
    nvrhi::BindingSetHandle m_gradientMipmapBindingSet0; // offset 1544
    nvrhi::BindingSetHandle m_gradientMipmapBindingSet1; // offset 1552
    nvrhi::BindingSetHandle m_momentMipmapBindingSet0; // offset 1560
    nvrhi::BindingSetHandle m_momentMipmapBindingSet1; // offset 1568
    nvrhi::BindingLayoutHandle m_mipmapBindingLayout; // offset 1576

    float m_timerResultsMs[4]; // offset 1584
    uint32_t m_timerReadIndex; // offset 1600
    uint32_t m_timerWriteIndex; // offset 1604
    nvrhi::TimerQueryHandle m_timerQueries[4]; // offset 1608
};

struct WindWavesSimulationLayoutVerifier
{
    static_assert(offsetof(Wind_Waves_Simulation::CascadeState, simulation) == 0);
    static_assert(offsetof(Wind_Waves_Simulation::CascadeState, displacementMapVersion) == 8);
    static_assert(offsetof(Wind_Waves_Simulation::CascadeState, gpuSlotDirty) == 16);
    static_assert(offsetof(Wind_Waves_Simulation::CascadeState, gradientTextures) == 24);
    static_assert(offsetof(Wind_Waves_Simulation::CascadeState, momentTextures) == 56);
    static_assert(offsetof(Wind_Waves_Simulation::CascadeState, foamEnergyTexture) == 88);
    static_assert(offsetof(Wind_Waves_Simulation::CascadeState, gradientBindingSets) == 96);
    static_assert(offsetof(Wind_Waves_Simulation::CascadeState, momentBindingSets) == 128);

    static_assert(offsetof(Wind_Waves_Simulation, m_foamWhitecapsThreshold) == 0);
    static_assert(offsetof(Wind_Waves_Simulation, m_cascades) == 4);
    static_assert(offsetof(Wind_Waves_Simulation, m_timeScale) == 516);
    static_assert(offsetof(Wind_Waves_Simulation, m_numGPUs) == 520);
    static_assert(offsetof(Wind_Waves_Simulation, m_simulationAPI) == 524);
    static_assert(offsetof(Wind_Waves_Simulation, m_cpuThreadingModel) == 528);
    static_assert(offsetof(Wind_Waves_Simulation, m_enableGpuTimers) == 532);
    static_assert(offsetof(Wind_Waves_Simulation, m_enableCpuTimers) == 533);
    static_assert(offsetof(Wind_Waves_Simulation, m_enableGpuDrivenDisplacementCalculation) == 534);
    static_assert(offsetof(Wind_Waves_Simulation, m_padding0) == 535);
    static_assert(offsetof(Wind_Waves_Simulation, m_cascadeStates) == 536);
    static_assert(offsetof(Wind_Waves_Simulation, m_numGpuSlots) == 1176);
    static_assert(offsetof(Wind_Waves_Simulation, m_gpuSlotIndex) == 1180);
    static_assert(offsetof(Wind_Waves_Simulation, m_simulationTime) == 1184);
    static_assert(offsetof(Wind_Waves_Simulation, m_simulationTimeHistory) == 1192);
    static_assert(offsetof(Wind_Waves_Simulation, m_simulationTimeHistoryCount) == 1232);
    static_assert(offsetof(Wind_Waves_Simulation, m_simulationTimeHistoryPadding) == 1236);
    static_assert(offsetof(Wind_Waves_Simulation, m_simulationDeltaTime) == 1240);
    static_assert(offsetof(Wind_Waves_Simulation, m_manager) == 1248);
    static_assert(offsetof(Wind_Waves_Simulation, m_threadPool) == 1256);
    static_assert(offsetof(Wind_Waves_Simulation, m_messageCallback) == 1264);
    static_assert(offsetof(Wind_Waves_Simulation, m_device) == 1272);
    static_assert(offsetof(Wind_Waves_Simulation, m_commandList) == 1280);
    static_assert(offsetof(Wind_Waves_Simulation, m_enableGraphics) == 1288);
    static_assert(offsetof(Wind_Waves_Simulation, m_padding1) == 1289);
    static_assert(offsetof(Wind_Waves_Simulation, m_displacementTextureArray) == 1296);
    static_assert(offsetof(Wind_Waves_Simulation, m_gradientTextureArray) == 1304);
    static_assert(offsetof(Wind_Waves_Simulation, m_momentTextureArray) == 1312);
    static_assert(offsetof(Wind_Waves_Simulation, m_gradientFoamShader) == 1320);
    static_assert(offsetof(Wind_Waves_Simulation, m_renderingConstantBuffer) == 1328);
    static_assert(offsetof(Wind_Waves_Simulation, m_gradientPipeline) == 1336);
    static_assert(offsetof(Wind_Waves_Simulation, m_gradientBindingLayout) == 1344);
    static_assert(offsetof(Wind_Waves_Simulation, m_foamInjectionShader) == 1352);
    static_assert(offsetof(Wind_Waves_Simulation, m_foamInjectionPipeline) == 1360);
    static_assert(offsetof(Wind_Waves_Simulation, m_foamInjectionBindingLayout) == 1368);
    static_assert(offsetof(Wind_Waves_Simulation, m_wrapSampler) == 1376);
    static_assert(offsetof(Wind_Waves_Simulation, m_gpuSampleShader) == 1384);
    static_assert(offsetof(Wind_Waves_Simulation, m_readbackPositionsBuffer) == 1392);
    static_assert(offsetof(Wind_Waves_Simulation, m_readbackResultsBuffer) == 1400);
    static_assert(offsetof(Wind_Waves_Simulation, m_readbackResultsBufferStaging) == 1408);
    static_assert(offsetof(Wind_Waves_Simulation, m_calculateReadbacksCB) == 1416);
    static_assert(offsetof(Wind_Waves_Simulation, m_gpuSamplePipeline) == 1424);
    static_assert(offsetof(Wind_Waves_Simulation, m_gpuSampleBindingLayout) == 1432);
    static_assert(offsetof(Wind_Waves_Simulation, m_gpuSampleBindingSet) == 1440);
    static_assert(offsetof(Wind_Waves_Simulation, m_textureArrayShader) == 1448);
    static_assert(offsetof(Wind_Waves_Simulation, m_textureArrayMipmapConstantBuffer) == 1456);
    static_assert(offsetof(Wind_Waves_Simulation, m_textureArrayPipeline) == 1464);
    static_assert(offsetof(Wind_Waves_Simulation, m_textureArrayBindingSets) == 1472);
    static_assert(offsetof(Wind_Waves_Simulation, m_textureArrayBindingLayout) == 1504);
    static_assert(offsetof(Wind_Waves_Simulation, m_mipmapShader) == 1512);
    static_assert(offsetof(Wind_Waves_Simulation, m_mipmapPipeline) == 1520);
    static_assert(offsetof(Wind_Waves_Simulation, m_displacementMipmapBindingSet0) == 1528);
    static_assert(offsetof(Wind_Waves_Simulation, m_displacementMipmapBindingSet1) == 1536);
    static_assert(offsetof(Wind_Waves_Simulation, m_gradientMipmapBindingSet0) == 1544);
    static_assert(offsetof(Wind_Waves_Simulation, m_gradientMipmapBindingSet1) == 1552);
    static_assert(offsetof(Wind_Waves_Simulation, m_momentMipmapBindingSet0) == 1560);
    static_assert(offsetof(Wind_Waves_Simulation, m_momentMipmapBindingSet1) == 1568);
    static_assert(offsetof(Wind_Waves_Simulation, m_mipmapBindingLayout) == 1576);
    static_assert(offsetof(Wind_Waves_Simulation, m_timerResultsMs) == 1584);
    static_assert(offsetof(Wind_Waves_Simulation, m_timerReadIndex) == 1600);
    static_assert(offsetof(Wind_Waves_Simulation, m_timerWriteIndex) == 1604);
    static_assert(offsetof(Wind_Waves_Simulation, m_timerQueries) == 1608);
};

static_assert(sizeof(Wind_Waves_Simulation) == 0x668,
              "Wind waves simulation must match recovered object size.");

#endif /* SRC_WWS_WIND_WAVES_SIMULATION_H */

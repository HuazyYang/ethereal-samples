#ifndef SRC_WWS_LOCAL_WAVES_SIMULATION_H
#define SRC_WWS_LOCAL_WAVES_SIMULATION_H

#include <cstdint>

#include <nvrhi/nvrhi.h>
#include <nvrhi/core/foundation.h>
#include <waveworks/GFSDK_WaveWorks.h>

#include "Local_Waves_FFT_Simulation_CPU_Impl.h"
#include "Local_Waves_FFT_Simulation_Compute_Impl.h"
#include "ThreadPool.h"
#include "Types.h"

struct Local_Waves_Simulation
{
    Local_Waves_Simulation();
    ~Local_Waves_Simulation();

    void addDisturbances(gfsdk_float4* disturbanceData, uint32_t numSamples);
    WWRESULT advanceReadbackCursor(bool block, bool& wouldBlock);
    WWRESULT advanceStagingCursor(bool block, bool& wouldBlock);
    WWRESULT allocateAll();
    WWRESULT allocateRenderingResources();
    WWRESULT allocateSimulation();
    WWRESULT consumeAvailableTimerSlot(uint32_t& slot);
    WWRESULT getDisplacements(
        const gfsdk_float2* samplePoints,
        gfsdk_float4* displacements,
        uint32_t numSamples,
        bool calculateOnGPU);
    bool getReadbackCursor(uint64_t* cursor) const;
    WWRESULT getRenderingResources(GFSDK_WaveWorks_Local_Waves_Rendering_Data& data) const;
    bool getStagingCursor(uint64_t* cursor) const;
    WWRESULT getStats(GFSDK_WaveWorks_Local_Waves_Simulation_Stats& stats) const;
    WWRESULT init11(
        void* deviceContext,
        const Local_Waves_Detailed_Simulation_Parameters& params,
        bool enableGraphics);
    WWRESULT init12(
        void* device,
        void* queue,
        const Local_Waves_Detailed_Simulation_Parameters& params,
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
        const Local_Waves_Detailed_Simulation_Parameters& params,
        bool enableGraphics);
    WWRESULT kick(uint64_t* kickId);
    WWRESULT reinit(const Local_Waves_Detailed_Simulation_Parameters& params);
    void releaseAll();
    void releaseRenderingResources();
    void releaseSimulation();
    void resetSimulation();
    void setSimulationDeltaTime(float deltaTime);
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

    void allocateThreadPool(GFSDK_WaveWorks_Simulation_CPU_Threading_Model threadingModel);
    void freeDisturbanceBuffers();
    void resetTimerRing();

    float m_deltaTime; // offset 0
    Local_Waves_Detailed_Simulation_Parameters m_params; // offset 4
    Local_Waves_FFT_Simulation* m_simulation; // offset 64
    uint64_t m_displacementMapVersion; // offset 72
    bool m_renderingResetPending; // offset 80
    bool m_simulationResetPending; // offset 81
    ThreadPool* m_threadPool; // offset 88
    float2* m_disturbanceBuffers[2]; // offset 96
    uint32_t m_disturbanceWriteIndex; // offset 112
    nvrhi::AutoPtr<MessageCallback> m_messageCallback = MAKE_RC_OBJ_PTR(MessageCallback); // offset 120
    nvrhi::DeviceHandle m_device; // offset 128
    nvrhi::CommandListHandle m_commandList; // offset 136
    bool m_enableGraphics; // offset 144

    nvrhi::TextureHandle m_foamTexture; // offset 152
    nvrhi::TextureHandle m_gradientTexture; // offset 160
    nvrhi::ShaderHandle m_gradientFoamShader; // offset 168
    nvrhi::BufferHandle m_gradientConstantBuffer; // offset 176
    nvrhi::ComputePipelineHandle m_gradientFoamPipeline; // offset 184
    nvrhi::BindingLayoutHandle m_gradientFoamBindingLayout; // offset 192
    nvrhi::BindingSetHandle m_gradientFoamBindingSet; // offset 200
    nvrhi::ShaderHandle m_foamInjectionShader; // offset 208
    nvrhi::ComputePipelineHandle m_foamInjectionPipeline; // offset 216
    nvrhi::BindingLayoutHandle m_foamInjectionBindingLayout; // offset 224
    nvrhi::BindingSetHandle m_foamInjectionBindingSet; // offset 232
    nvrhi::SamplerHandle m_wrapSampler; // offset 240
    nvrhi::SamplerHandle m_borderSampler; // offset 248
    nvrhi::ShaderHandle m_gpuSampleShader; // offset 256
    nvrhi::BufferHandle m_readbackPositionsBuffer; // offset 264
    nvrhi::BufferHandle m_readbackResultsBuffer; // offset 272
    nvrhi::BufferHandle m_readbackResultsBufferStaging; // offset 280
    nvrhi::BufferHandle m_calculateReadbacksCB; // offset 288
    nvrhi::ComputePipelineHandle m_gpuSamplePipeline; // offset 296
    nvrhi::BindingLayoutHandle m_gpuSampleBindingLayout; // offset 304
    nvrhi::BindingSetHandle m_gpuSampleBindingSet; // offset 312
    nvrhi::BufferHandle m_mipmapConstantBuffer; // offset 320
    nvrhi::ShaderHandle m_mipmapShader; // offset 328
    nvrhi::ComputePipelineHandle m_mipmapPipeline; // offset 336
    nvrhi::BindingSetHandle m_displacementMipmapBindingSet0; // offset 344
    nvrhi::BindingSetHandle m_displacementMipmapBindingSet1; // offset 352
    nvrhi::BindingSetHandle m_gradientMipmapBindingSet0; // offset 360
    nvrhi::BindingSetHandle m_gradientMipmapBindingSet1; // offset 368
    nvrhi::BindingLayoutHandle m_mipmapBindingLayout; // offset 376

    float m_timerResultsMs[4]; // offset 384
    uint32_t m_timerReadIndex; // offset 400
    uint32_t m_timerWriteIndex; // offset 404
    nvrhi::TimerQueryHandle m_timerQueries[4]; // offset 408
};

static_assert(sizeof(Local_Waves_Simulation) == 0x1B8,
              "Local waves simulation must match recovered object size.");

// TODO: add member offset static check for @class Local_Waves_Simulation

#endif /* SRC_WWS_LOCAL_WAVES_SIMULATION_H */

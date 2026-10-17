#ifndef SRC_WWS_LOCAL_WAVES_FFT_SIMULATION_COMPUTE_IMPL_H
#define SRC_WWS_LOCAL_WAVES_FFT_SIMULATION_COMPUTE_IMPL_H

#include <cstdint>

#include "Local_Waves_FFT_Simulation_CPU_Impl.h"

class Local_Waves_FFT_Simulation_Compute_Impl final : public Local_Waves_FFT_Simulation
{
public:
    explicit Local_Waves_FFT_Simulation_Compute_Impl(
        const Local_Waves_Detailed_Simulation_Parameters& params);
    ~Local_Waves_FFT_Simulation_Compute_Impl() override;

    WWRESULT advanceReadbackCursor(bool block) override;
    WWRESULT advanceStagingCursor(bool block) override;
    WWRESULT beforeReinit() override;
    WWRESULT beforeReallocateSimulation();
    WWRESULT canCollectSingleReadbackResultWithoutBlocking();
    WWRESULT collectSingleReadbackResult(bool block);
    WWRESULT consumeAvailableReadbackSlot(uint32_t& slot, uint64_t kickId);
    WWRESULT consumeAvailableTimerSlot(uint32_t& slot, uint64_t kickId);

    nvrhi::TextureHandle getDisplacementMap() const override;
    uint64_t getDisplacementMapVersion() const override;
    bool getReadbackCursor(uint64_t* cursor) const override;
    bool getStagingCursor(uint64_t* cursor) const override;
    WWRESULT getTimings(GFSDK_WaveWorks_Local_Waves_Simulation_Stats& stats) const override;
    bool hasReadbacksInFlight() const;

    WWRESULT init(
        nvrhi::IDevice* device,
        nvrhi::CommandListHandle cmdList,
        bool useCpu) override;
    WWRESULT kick(float simulationTime, float2* disturbanceData, uint64_t& kickId) override;
    WWRESULT mapDisplacementData(
        uint8_t** data,
        uint32_t* rowPitch,
        bool* fp16) override;
    WWRESULT reinit(const Local_Waves_Detailed_Simulation_Parameters& params) override;
    void releaseAllResources();
    void reset() override;
    WWRESULT resetReadbacks();
    void unmapDisplacementData() override;
    WWRESULT waitForAllInFlightReadbacks();
    WWRESULT waitForAllInFlightTimers();
    WWRESULT waitStagingCursor() override;

private:
    struct DisturbanceUpload
    {
        float row; // offset 0
        float column; // offset 4
        float displacement; // offset 8
        float velocityPotential; // offset 12
    };

    WWRESULT allocateAllResources();
    void clearComputeBuffers();
    uint32_t gatherDisturbances(float2* disturbanceData);
    nvrhi::TextureSlice fullResolutionSlice() const;
    void releasePipelineResources();

    Local_Waves_Detailed_Simulation_Parameters m_params; // offset 8
    uint32_t m_padding0; // offset 68
    uint32_t m_readbackReadIndex; // offset 72
    uint32_t m_readbackWriteIndex; // offset 76
    bool m_readbacksReady; // offset 80
    char m_padding1[7]; // offset 81
    uint64_t m_readbackKickIds[4]; // offset 88
    uint64_t m_displacementMapVersion; // offset 120
    float m_timerResultsMs[4]; // offset 128
    uint32_t m_timerReadIndex; // offset 144
    uint32_t m_timerWriteIndex; // offset 148
    uint64_t m_nextKickId; // offset 152
    bool m_stagingCursorValid; // offset 160
    char m_padding2[7]; // offset 161
    uint64_t m_stagingCursorKickId; // offset 168
    bool m_resetPending; // offset 176
    char m_padding3[7]; // offset 177
    DisturbanceUpload* m_disturbanceScratch; // offset 184

    nvrhi::DeviceHandle m_device; // offset 192
    nvrhi::CommandListHandle m_cmdList; // offset 200
    nvrhi::TextureHandle m_displacementTexture; // offset 208
    nvrhi::BufferHandle m_disturbanceBuffer; // offset 216
    nvrhi::StagingTextureHandle m_readbackStaging[4]; // offset 224
    nvrhi::EventQueryHandle m_readbackEvents[4]; // offset 256
    nvrhi::StagingTextureHandle m_currentReadbackStaging; // offset 288
    nvrhi::TimerQueryHandle m_timerQueries[4]; // offset 296

    nvrhi::ShaderHandle m_addDisturbancesShader; // offset 328
    nvrhi::ComputePipelineHandle m_addDisturbancesPipeline; // offset 336
    nvrhi::BindingSetHandle m_addDisturbancesBindingSet; // offset 344
    nvrhi::BindingLayoutHandle m_addDisturbancesBindingLayout; // offset 352

    nvrhi::ShaderHandle m_forwardRowsShader; // offset 360
    nvrhi::ComputePipelineHandle m_forwardRowsPipeline; // offset 368
    nvrhi::BindingSetHandle m_forwardRowsBindingSet; // offset 376
    nvrhi::BindingLayoutHandle m_forwardRowsBindingLayout; // offset 384

    nvrhi::ShaderHandle m_forwardColumnsShader; // offset 392
    nvrhi::ComputePipelineHandle m_forwardColumnsPipeline; // offset 400
    nvrhi::BindingSetHandle m_forwardColumnsBindingSet; // offset 408
    nvrhi::BindingLayoutHandle m_forwardColumnsBindingLayout; // offset 416

    nvrhi::ShaderHandle m_evolveShader; // offset 424
    nvrhi::ComputePipelineHandle m_evolvePipeline; // offset 432
    nvrhi::BindingSetHandle m_evolveBindingSet; // offset 440
    nvrhi::BindingLayoutHandle m_evolveBindingLayout; // offset 448

    nvrhi::ShaderHandle m_reverseRowsShader; // offset 456
    nvrhi::ComputePipelineHandle m_reverseRowsPipeline; // offset 464
    nvrhi::BindingSetHandle m_reverseRowsBindingSet; // offset 472
    nvrhi::BindingLayoutHandle m_reverseRowsBindingLayout; // offset 480

    nvrhi::ShaderHandle m_reverseColumnsShader; // offset 488
    nvrhi::ComputePipelineHandle m_reverseColumnsPipeline; // offset 496
    nvrhi::BindingSetHandle m_reverseColumnsBindingSet; // offset 504
    nvrhi::BindingLayoutHandle m_reverseColumnsBindingLayout; // offset 512

    nvrhi::BufferHandle m_verticalDisplacementAndPotentialBuffer; // offset 520
    nvrhi::BufferHandle m_verticalFrequencyBuffer; // offset 528
    nvrhi::BufferHandle m_lateralDisplacementBuffer; // offset 536
    nvrhi::BufferHandle m_lateralFrequencyBuffer; // offset 544
    nvrhi::BufferHandle m_constantBuffer; // offset 552
    nvrhi::BufferHandle m_intermediateBuffer1; // offset 560
    nvrhi::BufferHandle m_intermediateBuffer2; // offset 568
};

static_assert(sizeof(Local_Waves_FFT_Simulation_Compute_Impl) == 0x240,
              "Local waves compute implementation must match recovered object size.");

#endif /* SRC_WWS_LOCAL_WAVES_FFT_SIMULATION_COMPUTE_IMPL_H */

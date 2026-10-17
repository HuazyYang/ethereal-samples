#ifndef SRC_WWS_WIND_WAVES_FFT_SIMULATION_COMPUTE_IMPL_H
#define SRC_WWS_WIND_WAVES_FFT_SIMULATION_COMPUTE_IMPL_H

#include <cstdint>

#include "Wind_Waves_FFT_Simulation_CPU_Impl.h"

class Wind_Waves_Simulation_Manager_Compute_Impl;

class Wind_Waves_FFT_Simulation_Compute_Impl final : public Wind_Waves_FFT_Simulation
{
public:
    struct ReadbackArchiveFIFOSlot
    {
        ReadbackArchiveFIFOSlot();
        ~ReadbackArchiveFIFOSlot();

        uint64_t frameIndex; // offset 0
        nvrhi::StagingTextureHandle staging; // offset 8
    };

    explicit Wind_Waves_FFT_Simulation_Compute_Impl(
        Wind_Waves_Simulation_Manager_Compute_Impl* manager,
        const Wind_Waves_Detailed_Simulation_Parameters::Cascade& cascade);
    ~Wind_Waves_FFT_Simulation_Compute_Impl() override;

    WWRESULT allocateAllResources();
    WWRESULT archiveDisplacements();
    WWRESULT canCollectSingleReadbackResultWithoutBlocking();
    WWRESULT collectSingleReadbackResult(bool block);
    WWRESULT consumeAvailableReadbackSlot(uint32_t& slot, uint64_t kickId);
    WWRESULT consumeAvailableTimerSlot(uint32_t& slot, uint64_t kickId);

    WWRESULT getDisplacementArchiveCoords(
        float time,
        uint32_t* prevIndex,
        uint32_t* nextIndex,
        float* interpolation) const;

    nvrhi::TextureHandle getDisplacementMap() const;
    uint64_t getDisplacementMapVersion() const;
    bool getReadbackCursor(uint64_t* cursor) const;
    float getSimulationTime() const;
    bool hasReadbacksInFlight() const;

    WWRESULT init(
        nvrhi::IDevice* device,
        nvrhi::CommandListHandle cmdList,
        bool useCompute);
    WWRESULT initGaussAndOmega();
    WWRESULT kick(double simulationTime, uint64_t kickId);

    WWRESULT mapDisplacementArchiveData(
        uint8_t** data,
        uint32_t* rowPitch,
        uint32_t age,
        bool* outReadOnly);

    WWRESULT mapDisplacementData(
        uint8_t** data,
        uint32_t* rowPitch,
        bool* outReadOnly);

    WWRESULT reinit(const Wind_Waves_Detailed_Simulation_Parameters::Cascade& cascade);
    void releaseAllResources();
    WWRESULT resetReadbacks();
    void unmapDisplacementArchiveData(uint32_t age);
    void unmapDisplacementData();
    void updateConstantBuffer(double simulationTime);
    WWRESULT waitForAllInFlightReadbacks();
    WWRESULT waitForAllInFlightTimers();

private:
    struct ReadbackArchiveFIFO
    {
        ReadbackArchiveFIFOSlot* entries; // offset 0
        uint32_t capacity; // offset 8
        uint32_t startIndex; // offset 12
        uint32_t count; // offset 16
        uint32_t reserved; // offset 20
    };

    nvrhi::TextureSlice fullResolutionSlice() const;
    void releasePipelineResources();

    Wind_Waves_Simulation_Manager_Compute_Impl* m_manager; // offset 8
    Wind_Waves_Detailed_Simulation_Parameters::Cascade m_params; // offset 16

    uint32_t m_fftSize; // offset 144
    uint32_t m_halfFFTSizePlusOne; // offset 148
    bool m_clearDisplacementTexture; // offset 152
    bool m_gaussOmegaReady; // offset 153
    bool m_refreshH0Pending; // offset 154
    char m_padding0[1]; // offset 155
    uint32_t m_readbackReadIndex; // offset 156
    uint32_t m_readbackWriteIndex; // offset 160
    bool m_readbacksReady; // offset 164
    char m_padding1[3]; // offset 165
    uint64_t m_readbackKickIds[4]; // offset 168
    uint64_t m_displacementMapVersion; // offset 200
    float m_timerResultsMs[4]; // offset 208
    uint32_t m_timerReadIndex; // offset 224
    uint32_t m_timerWriteIndex; // offset 228

    nvrhi::DeviceHandle m_device; // offset 232
    nvrhi::CommandListHandle m_cmdList; // offset 240
    nvrhi::BufferHandle m_gaussBuffer; // offset 248
    nvrhi::BufferHandle m_omegaBuffer; // offset 256
    nvrhi::BufferHandle m_h0Buffer; // offset 264
    nvrhi::BufferHandle m_htBuffer; // offset 272
    nvrhi::BufferHandle m_dtBuffer; // offset 280
    nvrhi::TextureHandle m_displacementTexture; // offset 288
    nvrhi::BufferHandle m_constantBuffer; // offset 296
    nvrhi::StagingTextureHandle m_readbackStaging[4]; // offset 304
    nvrhi::EventQueryHandle m_readbackEvents[4]; // offset 336
    nvrhi::StagingTextureHandle m_currentReadbackStaging; // offset 368

    nvrhi::ComputePipelineHandle m_updateH0Pipeline; // offset 376
    nvrhi::BindingSetHandle m_updateH0BindingSet; // offset 384
    nvrhi::BindingLayoutHandle m_updateH0BindingLayout; // offset 392
    nvrhi::ComputePipelineHandle m_updateColumnsPipeline; // offset 400
    nvrhi::BindingSetHandle m_updateColumnsBindingSet; // offset 408
    nvrhi::BindingLayoutHandle m_updateColumnsBindingLayout; // offset 416
    nvrhi::ComputePipelineHandle m_updateHtPipeline; // offset 424
    nvrhi::BindingSetHandle m_updateHtBindingSet; // offset 432
    nvrhi::BindingLayoutHandle m_updateHtBindingLayout; // offset 440
    ReadbackArchiveFIFO* m_readbackArchive; // offset 448
    nvrhi::TimerQueryHandle m_timerQueries[4]; // offset 456
    nvrhi::ShaderHandle m_updateH0Shader; // offset 488
    nvrhi::ShaderHandle m_updateHtShader; // offset 496
    nvrhi::ShaderHandle m_updateColumnsShader; // offset 504
};

static_assert(sizeof(Wind_Waves_FFT_Simulation_Compute_Impl::ReadbackArchiveFIFOSlot) == 0x10,
              "Readback archive slots must match the recovered layout.");
static_assert(sizeof(Wind_Waves_FFT_Simulation_Compute_Impl) == 0x200,
              "Wind waves compute implementation must match recovered object size.");

#endif /* SRC_WWS_WIND_WAVES_FFT_SIMULATION_COMPUTE_IMPL_H */

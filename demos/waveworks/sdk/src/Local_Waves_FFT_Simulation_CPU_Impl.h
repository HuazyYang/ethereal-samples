#ifndef SRC_WWS_LOCAL_WAVES_FFT_SIMULATION_CPU_IMPL_H
#define SRC_WWS_LOCAL_WAVES_FFT_SIMULATION_CPU_IMPL_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

#include <nvrhi/nvrhi.h>
#include <nvrhi/core/foundation.h>
#include <waveworks/GFSDK_WaveWorks.h>

#include "ThreadPool.h"
#include "Types.h"

class Local_Waves_FFT_Simulation
{
public:
    virtual ~Local_Waves_FFT_Simulation() = default;
    virtual WWRESULT beforeReinit() = 0;
    virtual WWRESULT kick(float simulationTime, float2* disturbanceData, uint64_t& kickId) = 0;
    virtual bool getStagingCursor(uint64_t* cursor) const = 0;
    virtual WWRESULT advanceStagingCursor(bool block) = 0;
    virtual bool getReadbackCursor(uint64_t* cursor) const = 0;
    virtual WWRESULT advanceReadbackCursor(bool block) = 0;
    virtual WWRESULT waitStagingCursor() = 0;
    virtual WWRESULT getTimings(
        GFSDK_WaveWorks_Local_Waves_Simulation_Stats& stats) const = 0;
    virtual WWRESULT init(
        nvrhi::IDevice* device,
        nvrhi::CommandListHandle cmdList,
        bool useCpu) = 0;
    virtual WWRESULT reinit(const Local_Waves_Detailed_Simulation_Parameters& params) = 0;
    virtual uint64_t getDisplacementMapVersion() const = 0;
    virtual nvrhi::TextureHandle getDisplacementMap() const = 0;
    virtual WWRESULT mapDisplacementData(
        uint8_t** data,
        uint32_t* rowPitch,
        bool* fp16) = 0;
    virtual void unmapDisplacementData() = 0;
    virtual void reset() = 0;
};

class Local_Waves_FFT_Simulation_CPU_Impl : public Local_Waves_FFT_Simulation
{
public:
    Local_Waves_FFT_Simulation_CPU_Impl(
        const Local_Waves_Detailed_Simulation_Parameters& params,
        ThreadPool* threadPool);
    ~Local_Waves_FFT_Simulation_CPU_Impl() override;

    WWRESULT advanceReadbackCursor(bool block) override;
    WWRESULT advanceStagingCursor(bool block) override;
    WWRESULT beforeReinit() override;
    void completeSimulationStep(uint64_t frameIndex);

    nvrhi::TextureHandle getDisplacementMap() const override;
    uint64_t getDisplacementMapVersion() const override;
    int64_t getNumApplyEvolvingOperatorsTasks() const;
    int64_t getNumFFTTasks() const;
    int64_t getNumGenerateEvolvingOperatorsTasks() const;
    int64_t getNumGenerateTextureTasks() const;
    bool getReadbackCursor(uint64_t* cursor) const override;
    bool getStagingCursor(uint64_t* cursor) const override;
    WWRESULT getTimings(GFSDK_WaveWorks_Local_Waves_Simulation_Stats& stats) const override;
    int64_t getTotalNumberOfTasks() const;

    WWRESULT init(
        nvrhi::IDevice* device,
        nvrhi::CommandListHandle cmdList,
        bool useCpu) override;
    WWRESULT kick(float simulationTime, float2* disturbanceData, uint64_t& kickId) override;
    WWRESULT mapDisplacementData(
        uint8_t** data,
        uint32_t* rowPitch,
        bool* fp16) override;
    WWRESULT prepareSimulationStep();
    WWRESULT reinit(const Local_Waves_Detailed_Simulation_Parameters& params) override;
    void releaseAllResources();
    void reset() override;
    void unmapDisplacementData() override;
    WWRESULT waitStagingCursor() override;
    void waitTasksCompletion();

private:
    void applyEvolvingOperators(uint startRow, uint rowCount);
    void applyEvolvingOperatorsTask(uint taskIndex);
    WWRESULT allocateAllResources();
    void computeFFT_X_NForwardTask(uint taskIndex);
    void computeFFT_X_NReverseTask(uint taskIndex);
    void computeFFT_Y_NForwardTask(uint taskIndex);
    void computeFFT_Y_NReverseTask(uint taskIndex);
    void flip();
    void generateDisplacementData(uint startRow, uint rowCount);
    void generateDisplacementDataTask(uint taskIndex);
    void generateEvolvingOperators(uint startRow, uint rowCount);
    void generateEvolvingOperatorsTask(uint taskIndex);
    void resetTaskCounters();
    void uploadCompletedBuffer(uint64_t version);

    float m_simulationTime; // offset 8
    Local_Waves_Detailed_Simulation_Parameters m_params; // offset 12
    ThreadPool* m_threadPool; // offset 72
    bool m_enableCpuTimers; // offset 80

    uint64_t m_mainThreadWaitNs; // offset 88
    std::atomic<int64_t> m_currentWorkerThreadNs; // offset 96
    uint64_t m_completedWorkerThreadNs; // offset 104

    std::atomic<int32_t> m_numFFT_X_NForwardTasks; // offset 112
    std::atomic<int32_t> m_numFFT_Y_NForwardTasks; // offset 116
    std::atomic<int32_t> m_numGenerateEvolvingOperatorsTasks; // offset 120
    std::atomic<int32_t> m_numApplyEvolvingOperatorsTasks; // offset 124
    std::atomic<int32_t> m_numFFT_X_NReverseTasks; // offset 128
    std::atomic<int32_t> m_numFFT_Y_NReverseTasks; // offset 132
    std::atomic<int32_t> m_numGenerateTextureTasks; // offset 136
    uint32_t m_padding1; // offset 140

    uint64_t m_nextKickId; // offset 144
    bool m_tasksInFlight; // offset 152
    bool m_stagingPending; // offset 153
    uint64_t m_stagedKickId; // offset 160
    bool m_cursorValid; // offset 168
    uint64_t m_cursorKickId; // offset 176
    bool m_resetPending; // offset 184

    nvrhi::DeviceHandle m_device; // offset 192
    nvrhi::CommandListHandle m_cmdList; // offset 200
    bool m_useCpu; // offset 208
    nvrhi::TextureHandle m_displacementTexture; // offset 216
    nvrhi::StagingTextureHandle m_staging[2]; // offset 224

    float2* m_initialX; // offset 240
    float2* m_initialY; // offset 248
    float2* m_height; // offset 256
    float2* m_slope; // offset 264
    float2* m_displacementX; // offset 272
    float2* m_displacementY; // offset 280
    uint16_t* m_outputFp16[2]; // offset 288
    uint32_t m_outputRowPitch; // offset 304
    float4* m_displacementBuffers[2]; // offset 312
    uint8_t* m_currentDisplacementBuffer; // offset 328
    uint32_t m_currentDisplacementBufferIndex; // offset 336
    float* m_operatorSinOmega; // offset 344
    float* m_operatorCos; // offset 352
    float* m_operatorSinInvOmega; // offset 360
    uint64_t m_displacementMapVersion; // offset 368
};

static_assert(sizeof(Local_Waves_FFT_Simulation_CPU_Impl) == 0x178,
              "Local waves CPU implementation must match recovered object size.");

#endif /* SRC_WWS_LOCAL_WAVES_FFT_SIMULATION_CPU_IMPL_H */

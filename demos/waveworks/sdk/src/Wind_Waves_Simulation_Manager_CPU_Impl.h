#ifndef SRC_WWS_WIND_WAVES_SIMULATION_MANAGER_CPU_IMPL_H
#define SRC_WWS_WIND_WAVES_SIMULATION_MANAGER_CPU_IMPL_H

#include <cstdint>
#include <vector>

#include "ThreadPool.h"
#include "Wind_Waves_FFT_Simulation_CPU_Impl.h"
#include "Wind_Waves_Simulation_Manager.h"

class Wind_Waves_Simulation_Manager_CPU_Impl final : public Wind_Waves_Simulation_Manager
{
public:
    Wind_Waves_Simulation_Manager_CPU_Impl(
        const Wind_Waves_Detailed_Simulation_Parameters& params,
        ThreadPool* threadPool);
    ~Wind_Waves_Simulation_Manager_CPU_Impl() override;

    WWRESULT advanceReadbackCursor(bool block) override;
    WWRESULT advanceStagingCursor(bool block) override;
    WWRESULT archiveDisplacements() override;
    WWRESULT beforeReinit(
        const Wind_Waves_Detailed_Simulation_Parameters& params,
        bool allowSimulationUpdate) override;

    void computeFFT_X_NTask(uint32_t cascadeIndex, uint32_t taskY, uint32_t taskX);
    void computeFFT_Y_NTask(uint32_t cascadeIndex, uint32_t taskY, uint32_t taskX);
    Wind_Waves_FFT_Simulation* createSimulation(
        const Wind_Waves_Detailed_Simulation_Parameters::Cascade& cascade) override;
    void flip(bool alreadyOpen);
    bool getReadbackCursor(uint64_t* cursor) const override;
    bool getStagingCursor(uint64_t* cursor) const override;
    WWRESULT getTimings(GFSDK_WaveWorks_Wind_Waves_Simulation_Stats& stats) const override;
    WWRESULT kick(double simulationTime, uint64_t& kickId) override;
    void processWindWavesSimulationTask(
        uint32_t taskKind,
        uint32_t cascadeIndex,
        uint32_t taskY,
        uint32_t taskX);
    void releaseSimulation(Wind_Waves_FFT_Simulation* simulation) override;
    void updateH0Task(uint32_t cascadeIndex, uint32_t taskIndex);
    void updateHtTask(uint32_t cascadeIndex, uint32_t taskIndex);
    void updateTextureTask(uint32_t cascadeIndex, uint32_t taskIndex);
    WWRESULT waitStagingCursor() override;
    void waitTasksCompletion();

private:
    using SimulationList = std::vector<Wind_Waves_FFT_Simulation_CPU_Impl*>;

    WWRESULT completePendingSimulation();

    SimulationList m_simulations; // offset 8
    uint64_t m_nextKickId; // offset 32
    bool m_tasksInFlight; // offset 40
    bool m_completionPending; // offset 41
    char m_padding0[6]; // offset 42
    uint64_t m_inFlightKickId; // offset 48
    bool m_readbackCursorValid; // offset 56
    char m_padding1[7]; // offset 57
    uint64_t m_readbackCursorKickId; // offset 64
    ThreadPool* m_threadPool; // offset 72
    uint64_t m_cpuMainThreadWaitNs; // offset 80
    std::atomic<uint64_t> m_cpuThreadsTotalNs; // offset 88
    uint64_t m_lastCpuThreadsTotalNs; // offset 96
    bool m_enableCpuTimers; // offset 104
    char m_padding2[7]; // offset 105
};

static_assert(sizeof(Wind_Waves_Simulation_Manager_CPU_Impl) == 0x70,
              "Wind waves CPU manager must match recovered object size.");

#endif /* SRC_WWS_WIND_WAVES_SIMULATION_MANAGER_CPU_IMPL_H */

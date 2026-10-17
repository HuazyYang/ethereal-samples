#ifndef SRC_WWS_WIND_WAVES_SIMULATION_MANAGER_COMPUTE_IMPL_H
#define SRC_WWS_WIND_WAVES_SIMULATION_MANAGER_COMPUTE_IMPL_H

#include <cstdint>
#include <vector>

#include "Wind_Waves_FFT_Simulation_Compute_Impl.h"
#include "Wind_Waves_Simulation_Manager.h"

class Wind_Waves_Simulation_Manager_Compute_Impl final : public Wind_Waves_Simulation_Manager
{
public:
    Wind_Waves_Simulation_Manager_Compute_Impl();
    ~Wind_Waves_Simulation_Manager_Compute_Impl() override;

    void VerifyReadbackLockstep() const;

    WWRESULT advanceReadbackCursor(bool block) override;
    WWRESULT advanceStagingCursor(bool block) override;
    WWRESULT archiveDisplacements() override;
    WWRESULT beforeReallocateSimulation();
    WWRESULT beforeReinit(
        const Wind_Waves_Detailed_Simulation_Parameters& params,
        bool allowSimulationUpdate) override;
    WWRESULT checkForReadbackResults();

    Wind_Waves_FFT_Simulation* createSimulation(
        const Wind_Waves_Detailed_Simulation_Parameters::Cascade& cascade) override;
    bool getReadbackCursor(uint64_t* cursor) const override;
    bool getStagingCursor(uint64_t* cursor) const override;
    WWRESULT getTimings(GFSDK_WaveWorks_Wind_Waves_Simulation_Stats& stats) const override;
    WWRESULT kick(double simulationTime, uint64_t& kickId) override;
    void releaseSimulation(Wind_Waves_FFT_Simulation* simulation) override;
    WWRESULT waitStagingCursor() override;

private:
    using SimulationList = std::vector<Wind_Waves_FFT_Simulation_Compute_Impl*>;

    SimulationList m_simulations; // offset 8
    uint64_t m_nextKickId; // offset 32
    bool m_stagingCursorValid; // offset 40
    char m_padding0[7]; // offset 41
    uint64_t m_stagingCursorKickId; // offset 48
};

static_assert(sizeof(Wind_Waves_Simulation_Manager_Compute_Impl) == 0x38,
              "Wind waves compute manager must match recovered object size.");

#endif /* SRC_WWS_WIND_WAVES_SIMULATION_MANAGER_COMPUTE_IMPL_H */

#ifndef SRC_WWS_WIND_WAVES_SIMULATION_MANAGER_H
#define SRC_WWS_WIND_WAVES_SIMULATION_MANAGER_H

#include <cstdint>

#include <waveworks/GFSDK_WaveWorks.h>

#include "Types.h"

class Wind_Waves_FFT_Simulation;

class Wind_Waves_Simulation_Manager
{
public:
    virtual ~Wind_Waves_Simulation_Manager() = default;

    virtual Wind_Waves_FFT_Simulation* createSimulation(
        const Wind_Waves_Detailed_Simulation_Parameters::Cascade& cascade) = 0;
    virtual void releaseSimulation(Wind_Waves_FFT_Simulation* simulation) = 0;
    virtual WWRESULT kick(double simulationTime, uint64_t& kickId) = 0;
    virtual bool getStagingCursor(uint64_t* cursor) const = 0;
    virtual bool getReadbackCursor(uint64_t* cursor) const = 0;
    virtual WWRESULT advanceStagingCursor(bool block) = 0;
    virtual WWRESULT advanceReadbackCursor(bool block) = 0;
    virtual WWRESULT waitStagingCursor() = 0;
    virtual WWRESULT archiveDisplacements() = 0;
    virtual WWRESULT beforeReinit(
        const Wind_Waves_Detailed_Simulation_Parameters& params,
        bool allowSimulationUpdate) = 0;
    virtual WWRESULT getTimings(GFSDK_WaveWorks_Wind_Waves_Simulation_Stats& stats) const = 0;
};

#endif /* SRC_WWS_WIND_WAVES_SIMULATION_MANAGER_H */

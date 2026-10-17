#pragma once

#include <donut/app/DeviceManager.h>
#include <waveworks/GFSDK_WaveWorks.h>

#include <cstdint>
#include <optional>
#include <string>

enum class SimulationDeviceMode
{
    Cpu,
    Gpu
};

struct SimulationDeviceSelection
{
    SimulationDeviceMode mode = SimulationDeviceMode::Gpu;
    std::optional<uint32_t> adapterIndex;
};

enum class ReadbackMode
{
    Cpu,
    Gpu
};

enum class BeachTerrainMode
{
    Survey,
    Cove
};

struct ExampleOptions
{
    nvrhi::GraphicsAPI backend = nvrhi::GraphicsAPI::D3D11;
    bool updateWindWaves = true;
    bool updateLocalWaves = true;
    std::optional<SimulationDeviceSelection> simulationDevice;
    std::optional<ReadbackMode> readbackMode;
    std::optional<BeachTerrainMode> beachTerrain;
};

int ParseExampleOptions(int argc, char **argv, ExampleOptions &options);

void ApplySimulationConfiguration(
    const ExampleOptions& options,
    GFSDK_WaveWorks_Wind_Waves_Simulation_Settings& windSettings,
    GFSDK_WaveWorks_Local_Waves_Simulation_Settings& localSettings,
    int& readbackUsage);

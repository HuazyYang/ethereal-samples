#include "example_options.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <string_view>
#include <limits>

namespace
{
struct Option {
    char identifier;
    const char* name;
    const char* valueName;
    const char* description;
};

constexpr Option kOptions[] = {
    {'h', "help", nullptr, "Show this help text (also -h)"},
    {'r', "rhi", "BACKEND", "Select d3d11, d3d12, or vk (aliases: dx11, dx12, vulkan)"},
    {'w', "disable-wind-waves", nullptr, "Start with wind-wave updates disabled"},
    {'l', "disable-local-waves", nullptr, "Start with local-wave updates disabled"},
    {'s', "simulation-dev", "DEVICE", "Select cpu, gpu, or gpu-N with an adapter index"},
    {'b', "readback-mode", "MODE", "Select cpu or gpu displacement sampling"},
    {'t', "beach-terrain", "survey|cove", "Beach only: surveyed coast or procedural crescent cove"},
};

std::string Lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

bool ParseAdapterIndex(const std::string& text, uint32_t& value)
{
    if (text.empty())
        return false;
    uint32_t parsed = 0;
    for (unsigned char character : text)
    {
        if (!std::isdigit(character))
            return false;
        const uint32_t digit = static_cast<uint32_t>(character) - 48u;
        if (parsed > (std::numeric_limits<uint32_t>::max() - digit) / 10u)
            return false;
        parsed = parsed * 10u + digit;
    }
    value = parsed;
    return true;
}

void PrintExampleUsage(const char* program)
{
    std::printf("Usage: %s [options]\n", program);
    for (const auto& option : kOptions) {
        std::printf("  --%s%s%s\n      %s\n", option.name,
            option.valueName ? "=" : "", option.valueName ? option.valueName : "",
            option.description);
    }
}
}

int ParseExampleOptions(int argc, char** argv, ExampleOptions& options)
{
    bool seen[256] = {};
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--") {
            if (i + 1 < argc) {
                std::fprintf(stderr, "error: Unexpected positional argument: %s\n", argv[i + 1]);
                return -1;
            }
            break;
        }
        const bool shortHelp = argument == "-h";
        if (!shortHelp && argument.substr(0, 2) != "--") {
            std::fprintf(stderr, "error: Unexpected argument: %s\n", argv[i]);
            return -1;
        }
        const size_t equals = argument.find('=');
        const auto name = shortHelp ? std::string_view("help")
            : argument.substr(2, equals == std::string_view::npos ? equals : equals - 2);
        const Option* selected = nullptr;
        for (const auto& option : kOptions) {
            if (name == option.name) {
                selected = &option;
                break;
            }
        }
        if (!selected) {
            std::fprintf(stderr, "error: Unknown option: %s\n", argv[i]);
            return -1;
        }
        const char identifier = selected->identifier;
        const auto optionIndex = static_cast<unsigned char>(identifier);
        if (seen[optionIndex]) {
            std::fprintf(stderr, "error: Duplicate option: --%s\n", selected->name);
            return -1;
        }
        seen[optionIndex] = true;
        const char* valueText = nullptr;
        if (selected->valueName) {
            if (equals != std::string_view::npos)
                valueText = argv[i] + equals + 1;
            else if (i + 1 < argc && argv[i + 1][0] != '-')
                valueText = argv[++i];
            if (!valueText || !*valueText) {
                std::fprintf(stderr, "error: --%s requires %s\n", selected->name, selected->valueName);
                return -1;
            }
        } else if (equals != std::string_view::npos) {
            std::fprintf(stderr, "error: --%s does not accept a value\n", selected->name);
            return -1;
        }

        switch (identifier)
        {
        case 'h':
            PrintExampleUsage(argv[0]);
            std::exit(0);

        case 'w':
            options.updateWindWaves = false;
            break;

        case 'l':
            options.updateLocalWaves = false;
            break;

        case 's':
        {
            const std::string value = Lowercase(valueText);
            SimulationDeviceSelection selection;
            if (value == "cpu")
            {
                selection.mode = SimulationDeviceMode::Cpu;
            }
            else if (value == "gpu")
            {
                selection.mode = SimulationDeviceMode::Gpu;
            }
            else if (value.rfind("gpu-", 0) == 0)
            {
                uint32_t adapterIndex = 0;
                if (!ParseAdapterIndex(value.substr(4), adapterIndex))
                {
                    std::fprintf(stderr,
                        "error: --simulation-dev requires cpu, gpu, or gpu-N with a valid adapter index\n");
                    return -1;
                }
                selection.mode = SimulationDeviceMode::Gpu;
                selection.adapterIndex = adapterIndex;
            }
            else
            {
                std::fprintf(stderr, "error: --simulation-dev requires cpu, gpu, or gpu-N\n");
                return -1;
            }
            options.simulationDevice = selection;
            break;
        }

        case 'b':
        {
            const std::string value = Lowercase(valueText);
            if (value == "cpu") options.readbackMode = ReadbackMode::Cpu;
            else if (value == "gpu") options.readbackMode = ReadbackMode::Gpu;
            else
            {
                std::fprintf(stderr, "error: --readback-mode requires cpu or gpu\n");
                return -1;
            }
            break;
        }

        case 't':
        {
            const std::string value = Lowercase(valueText);
            if (value == "survey") options.beachTerrain = BeachTerrainMode::Survey;
            else if (value == "cove") options.beachTerrain = BeachTerrainMode::Cove;
            else
            {
                std::fprintf(stderr, "error: --beach-terrain requires survey or cove\n");
                return -1;
            }
            break;
        }

        case 'r':
        {
            const std::string value = Lowercase(valueText);
            if (value == "d3d11" || value == "dx11") options.backend = nvrhi::GraphicsAPI::D3D11;
            else if (value == "d3d12" || value == "dx12") options.backend = nvrhi::GraphicsAPI::D3D12;
            else if (value == "vk" || value == "vulkan") options.backend = nvrhi::GraphicsAPI::VULKAN;
            else
            {
                std::fprintf(stderr, "error: Unknown --rhi backend: %s\n", value.c_str());
                return -1;
            }
            break;
        }

        default:
            std::fprintf(stderr, "error: Unknown option: --%s\n", selected->name);
            return -1;
        }
    }

    return 0;
}

void ApplySimulationConfiguration(
    const ExampleOptions& options,
    GFSDK_WaveWorks_Wind_Waves_Simulation_Settings& windSettings,
    GFSDK_WaveWorks_Local_Waves_Simulation_Settings& localSettings,
    int& readbackUsage)
{
    if (options.simulationDevice)
    {
        const GFSDK_WaveWorks_Simulation_API api =
            options.simulationDevice->mode == SimulationDeviceMode::Cpu
                ? GFSDK_WaveWorks_Simulation_API_CPU
                : GFSDK_WaveWorks_Simulation_API_Compute;
        windSettings.simulation_api = api;
        localSettings.simulation_api = api;
    }

    if (options.readbackMode)
    {
        const bool useCpu = *options.readbackMode == ReadbackMode::Cpu;
        windSettings.enable_CPU_driven_displacement_calculation = useCpu;
        windSettings.enable_GPU_driven_displacement_calculation = !useCpu;
        localSettings.enable_CPU_driven_displacement_calculation = useCpu;
        localSettings.enable_GPU_driven_displacement_calculation = !useCpu;
        readbackUsage = useCpu ? 1 : 2;
    }
}

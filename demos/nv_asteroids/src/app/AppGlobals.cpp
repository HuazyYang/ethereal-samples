#include "app/AppGlobals.h"

namespace demo
{
    CommandLineOptions g_Options;

    uint32_t g_ReplayFrameIndex = 0;

    int    g_BenchmarkFrameCount = 0;
    double g_BenchmarkTotalTime = 0.0;

    bool g_SkipNextFrameLimit = true;
    bool g_PrintLoadingTime = true;

    float g_SceneFarDistance = 20000.f;

    std::mt19937 g_RandomEngine; // default seed 5489, as in the binary
}

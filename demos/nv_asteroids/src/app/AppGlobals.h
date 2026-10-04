#pragma once

// Process-wide options and state of the 2018 demo.
//
// Asteroids.exe keeps the command-line options in loose globals that WinMain (0x14003CC90) fills and that
// FeatureDemo, UIData's constructor (0x1400246A0) and the startup dialog (DialogFunc 0x14002E0D0) read.
// They are grouped here; each field notes its 2018 address. The window/device parameters
// (dword_1402D249C width, ... in the 2018 DeviceCreationParameters at 0x1402D2490) live in main.cpp.

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace demo
{
    struct CommandLineOptions
    {
        int  antiAliasing = 1;              // dword_1402D10A4, "-aa N" (UIData::aaMode = (N == 1))
        bool enableMipmaps = true;          // byte_1402D10A8, cleared by "-disableMipmaps"
        bool enableShadows = true;          // byte_1402D10A9, cleared by "-disableShadows"
        bool enableToneMapping = true;      // byte_1402D10AA, cleared by "-disableToneMapping"
        bool enableSsao = true;             // byte_1402D10AB, cleared by "-disableSSAO"
        int  replayIndex = -1;              // dword_1402D10AC, "-replay N" -> media/replay.N.json
        int  viewIndex = -1;                // dword_1402D10B0, "-view N"   -> camera preset N
        std::wstring sceneName = L"asteroids.fscene"; // qword_1402D10D8, "-scene NAME"
                                            // unresolved: parsed but never read; FeatureDemo always loads "asteroids.fscene".
        bool renderLightProbes = false;     // byte_1402DF20D, "-renderLightProbes"
        bool benchmark = false;             // byte_1402DF20E, "-benchmark" (also the dialog's "Run benchmark" box)

        // Developer options (deviation, not in the binary):
        std::wstring probeOutputDir;        // "-probeOutput DIR": also write the -renderLightProbes results to a native folder
        std::wstring shaderOverrideDir;     // "-shaderOverride DIR": look in DIR/{demo,framework}/*.bin first (see tools/nvsp2shadermake.py)
        bool perfLog = false;               // "-perfLog": log the average frame time every 1000 frames
        std::vector<std::string> uiOverrides;   // "-set field=value": UIData settings (see app/UIOverrides.h)
        bool gpuProfile = false;            // "-gpuProfile": also log the GPU time of every pass every 1000 frames
        std::wstring dumpGBufferPrefix;     // "-dumpGBuffer PREFIX": with -screenshot, also save PREFIX_gb0/1/2.png
        int  showLightProbe = -1;           // "-showLightProbe N": enable the light-probe debug view (UIData+190/+192) for probe N
    };

    extern CommandLineOptions g_Options;

    // Replay frame counter (dword_1402D10A0). Incremented at the top of every RenderScene and used as
    // the index into the recorded input stream; reset to 0 when the opening sequence restarts a playback.
    extern uint32_t g_ReplayFrameIndex;

    // Benchmark accumulators (dword_1402DF218 / qword_1402DF210): number of frames and their summed
    // elapsed time while a benchmark replay plays back. Shown by the "Benchmark Result" popup.
    extern int    g_BenchmarkFrameCount;
    extern double g_BenchmarkTotalTime;

    // The fixed-step frame limiter skips its wait on the very first limited frame (byte_1402D2500 = 1).
    extern bool g_SkipNextFrameLimit;

    // "Loading time: %.3f s" is printed once, by the first RenderScene after loading (byte_1402D2501 = 1).
    extern bool g_PrintLoadingTime;

    // Global RNG (std::mt19937 at dword_1402D1100). Re-seeded with the default seed (5489) when a replay
    // restarts so that particles and other random effects play back identically.
    extern std::mt19937 g_RandomEngine;

    // Far distance used by the planet/sun/scene passes (dword_1402D10B4, initially 20000.0f).
    // SceneLoaded raises it to the diagonal of the scene bounds when that is larger.
    extern float g_SceneFarDistance;

    // Rec.709 luminance weights stored next to it (0x1402D10B8..0x1402D10C0).
    constexpr float c_LuminanceWeights[3] = { 0.2126f, 0.7152f, 0.0722f };
}

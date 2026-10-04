// Asteroids.exe: WinMain (0x14003CC90) and the startup dialog procedure DialogFunc (0x14002E0D0).
//
// Built only into the Asteroids executable (the asteroids_core glob excludes this file, and the
// executable target defines ASTEROIDS_MAIN_EXE); the guard keeps it inert if it is ever globbed into a library.

#ifdef ASTEROIDS_MAIN_EXE

#include "app/AppGlobals.h"
#include "app/FeatureDemo.h"
#include "app/UIData.h"
#include "app/UIOverrides.h"
#include "app/resource.h"
#include "meshlets/MeshShaderMode.h"
#include "meshlets/NvMeshShaderApi.h"
#include "ui/UIRenderer.h"

#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shellapi.h>
#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")

// The original manifest requests common controls 6 (visual styles for the startup dialog).
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace
{
    // The 2018 DeviceCreationParameters lived in a global (0x1402D2490) because the dialog edits the
    // resolution and the full-screen flag in place.
    donut::app::DeviceCreationParameters g_DeviceParams;

    std::string WideToUtf8(const std::wstring& text)
    {
        if (text.empty())
            return std::string();
        const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
        std::string result(size_t(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), result.data(), size, nullptr, nullptr);
        return result;
    }

    void CenterOnDesktop(HWND window)
    {
        RECT desktopRect, windowRect, offsetRect;
        GetWindowRect(GetDesktopWindow(), &desktopRect);
        GetWindowRect(window, &windowRect);
        CopyRect(&offsetRect, &desktopRect);

        OffsetRect(&windowRect, -windowRect.left, -windowRect.top);
        OffsetRect(&offsetRect, -offsetRect.left, -offsetRect.top);
        OffsetRect(&offsetRect, -windowRect.right, -windowRect.bottom);

        SetWindowPos(window, nullptr, desktopRect.left + offsetRect.right / 2, desktopRect.top + offsetRect.bottom / 2,
            0, 0, SWP_NOSIZE);
    }
}

// Asteroids.exe: 0x14002E0D0
INT_PTR CALLBACK DialogFunc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_INITDIALOG:
    {
        CenterOnDesktop(dialog);

        CheckDlgButton(dialog, IDC_FULLSCREEN, g_DeviceParams.startFullscreen ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_BENCHMARK, demo::g_Options.benchmark ? BST_CHECKED : BST_UNCHECKED);

        // Unique display resolutions of the primary display, sorted by width, then height.
        std::vector<std::pair<int, int>> modes;
        DEVMODEW devMode = {};
        devMode.dmSize = sizeof(DEVMODEW);
        for (DWORD modeIndex = 0; EnumDisplaySettingsW(nullptr, modeIndex, &devMode); modeIndex++)
        {
            const std::pair<int, int> mode(int(devMode.dmPelsWidth), int(devMode.dmPelsHeight));
            if (std::find(modes.begin(), modes.end(), mode) == modes.end())
                modes.push_back(mode);
        }
        std::sort(modes.begin(), modes.end());

        char text[256];
        for (const auto& mode : modes)
        {
            snprintf(text, sizeof(text), "%d x %d", mode.first, mode.second);
            SendMessageA(GetDlgItem(dialog, IDC_RESOLUTION), CB_ADDSTRING, 0, LPARAM(text));
        }

        snprintf(text, sizeof(text), "%d x %d", int(g_DeviceParams.backBufferWidth), int(g_DeviceParams.backBufferHeight));
        SetWindowTextA(GetDlgItem(dialog, IDC_RESOLUTION), text);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
        {
            char text[256] = {};
            GetDlgItemTextA(dialog, IDC_RESOLUTION, text, sizeof(text));

            int width = 0, height = 0;
            if (sscanf(text, "%d x %d", &width, &height) != 2)
            {
                MessageBoxA(dialog, "Invalid resolution specification", "Error", MB_ICONERROR);
                return TRUE;
            }
            g_DeviceParams.backBufferWidth = uint32_t(width);
            g_DeviceParams.backBufferHeight = uint32_t(height);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        case IDC_FULLSCREEN:
            g_DeviceParams.startFullscreen = IsDlgButtonChecked(dialog, IDC_FULLSCREEN) != 0;
            return TRUE;
        case IDC_BENCHMARK:
            demo::g_Options.benchmark = IsDlgButtonChecked(dialog, IDC_BENCHMARK) != 0;
            return TRUE;
        default:
            return FALSE;
        }

    case WM_CLOSE:
        EndDialog(dialog, IDCANCEL);
        return TRUE;

    default:
        return FALSE;
    }
}

// Developer aid (deviation, not in the binary): on an unhandled exception, log a symbolized stack
// trace through donut::log and write Asteroids.dmp next to the executable.
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info)
{
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);

    donut::log::error("Unhandled exception 0x%08X at %p", info->ExceptionRecord->ExceptionCode,
        info->ExceptionRecord->ExceptionAddress);

    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    for (int depth = 0; depth < 48; depth++)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
            SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
            break;

        char symbolBuffer[sizeof(SYMBOL_INFO) + 512] = {};
        SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        IMAGEHLP_LINE64 line = { sizeof(IMAGEHLP_LINE64) };
        DWORD lineDisplacement = 0;
        const bool haveSymbol = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol);
        const bool haveLine = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line);
        donut::log::error("  #%02d %p %s+0x%llx %s:%lu", depth, (void*)frame.AddrPC.Offset,
            haveSymbol ? symbol->Name : "?", (unsigned long long)displacement,
            haveLine ? line.FileName : "", haveLine ? line.LineNumber : 0ul);
    }

    wchar_t dumpPath[MAX_PATH];
    GetModuleFileNameW(nullptr, dumpPath, MAX_PATH);
    wcscpy_s(wcsrchr(dumpPath, L'\\') + 1, MAX_PATH - (wcsrchr(dumpPath, L'\\') + 1 - dumpPath), L"Asteroids.dmp");
    HANDLE file = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION exceptionInfo = { GetCurrentThreadId(), info, FALSE };
        MiniDumpWriteDump(process, GetCurrentProcessId(), file, MiniDumpNormal, &exceptionInfo, nullptr, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// Asteroids.exe: 0x14003CC90
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
    using donut::app::DeviceManager;

    SetUnhandledExceptionFilter(CrashHandler);

    // Defaults of the 2018 globals: 1920x1080, 2 back buffers, SRGBA8 (2018 format 22), no MSAA,
    // feature level 12_1, windowed.
    g_DeviceParams.backBufferWidth = 1920;
    g_DeviceParams.backBufferHeight = 1080;
    g_DeviceParams.swapChainBufferCount = 2;
    g_DeviceParams.swapChainFormat = nvrhi::Format::SRGBA8_UNORM;
    g_DeviceParams.swapChainSampleCount = 1;
    g_DeviceParams.featureLevel = D3D_FEATURE_LEVEL_12_1;
    g_DeviceParams.startFullscreen = false;

    bool asyncLoad = true;
    bool showDialog = true;
    std::wstring adapterName;
    std::filesystem::path screenshotPath;
    uint32_t screenshotFrame = 0;
    uint32_t screenshotCount = 1;
    std::filesystem::path logPath;
    std::wstring meshShadersOption;     // "-meshShaders nvapi|d3d12" (deviation, see below)

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
    {
        OutputDebugStringW(L"CommandLineToArgvW failed\n");
        return 0;
    }

    // deviation: the binary reads argv[i + 1] without checking that it exists.
    auto nextArgument = [&](int& i) -> std::wstring
    {
        if (i + 1 >= argc)
            return std::wstring();
        return argv[++i];
    };

    for (int i = 0; i < argc; i++)
    {
        const wchar_t* arg = argv[i];

        if (lstrcmpW(arg, L"-width") == 0)
            g_DeviceParams.backBufferWidth = uint32_t(std::stoi(nextArgument(i)));
        else if (lstrcmpW(arg, L"-height") == 0)
            g_DeviceParams.backBufferHeight = uint32_t(std::stoi(nextArgument(i)));
        else if (lstrcmpW(arg, L"-scene") == 0)
            demo::g_Options.sceneName = nextArgument(i);
        else if (lstrcmpW(arg, L"-disableMipmaps") == 0)
            demo::g_Options.enableMipmaps = false;
        else if (lstrcmpW(arg, L"-disableShadows") == 0)
            demo::g_Options.enableShadows = false;
        else if (lstrcmpW(arg, L"-disableToneMapping") == 0)
            demo::g_Options.enableToneMapping = false;
        else if (lstrcmpW(arg, L"-disableSSAO") == 0)
            demo::g_Options.enableSsao = false;
        else if (lstrcmpW(arg, L"-disableAsync") == 0)
            asyncLoad = false;
        else if (lstrcmpW(arg, L"-debug") == 0)
        {
            g_DeviceParams.enableDebugRuntime = true;
            g_DeviceParams.enableNvrhiValidationLayer = true;
        }
        else if (lstrcmpW(arg, L"-aa") == 0)
            demo::g_Options.antiAliasing = std::stoi(nextArgument(i));
        else if (lstrcmpW(arg, L"-meshletDrawFile") == 0)
            nextArgument(i); // unresolved: parsed and discarded by the binary
        else if (lstrcmpW(arg, L"-adapter") == 0)
            adapterName = nextArgument(i);
        else if (lstrcmpW(arg, L"-fullscreen") == 0)
            g_DeviceParams.startFullscreen = true;
        else if (lstrcmpW(arg, L"-nodialog") == 0)
            showDialog = false;
        else if (lstrcmpW(arg, L"-replay") == 0)
            demo::g_Options.replayIndex = std::stoi(nextArgument(i));
        else if (lstrcmpW(arg, L"-benchmark") == 0)
            demo::g_Options.benchmark = true;
        else if (lstrcmpW(arg, L"-view") == 0)
            demo::g_Options.viewIndex = std::stoi(nextArgument(i));
        else if (lstrcmpW(arg, L"-renderLightProbes") == 0)
            demo::g_Options.renderLightProbes = true;
        else if (lstrcmpW(arg, L"-log") == 0)
        {
            // Developer option (deviation, not in the binary): "-log <file>" mirrors the donut log into a file
            // (without the message boxes of the default callback).
            logPath = nextArgument(i);
        }
        else if (lstrcmpW(arg, L"-probeOutput") == 0)
        {
            // Developer option (deviation): the 2018 build saved the captured probes through the read-only
            // media.db file system, which always failed; this also writes them to a native folder.
            demo::g_Options.probeOutputDir = nextArgument(i);
        }
        else if (lstrcmpW(arg, L"-shaderOverride") == 0)
        {
            // Developer option (deviation): shader blobs found in DIR/demo and DIR/framework replace the rebuilt
            // ones, e.g. the original 2018 shaders converted with tools/nvsp2shadermake.py.
            demo::g_Options.shaderOverrideDir = nextArgument(i);
        }
        else if (lstrcmpW(arg, L"-screenshotCount") == 0)
        {
            const std::wstring count = nextArgument(i);   // developer option (deviation): see -screenshot
            screenshotCount = count.empty() ? 1u : uint32_t(std::stoul(count));
        }
        else if (lstrcmpW(arg, L"-set") == 0)
        {
            const std::wstring assignment = nextArgument(i);   // developer option (deviation)
            demo::g_Options.uiOverrides.push_back(std::string(assignment.begin(), assignment.end()));
        }
        else if (lstrcmpW(arg, L"-dumpGBuffer") == 0)
        {
            demo::g_Options.dumpGBufferPrefix = nextArgument(i);  // developer option (deviation)
        }
        else if (lstrcmpW(arg, L"-gpuProfile") == 0)
        {
            demo::g_Options.gpuProfile = true;  // developer option (deviation)
        }
        else if (lstrcmpW(arg, L"-perfLog") == 0)
        {
            demo::g_Options.perfLog = true;  // developer option (deviation)
        }
        else if (lstrcmpW(arg, L"-showLightProbe") == 0)
        {
            // Developer option (deviation): turns on the light-probe debug view without the UI.
            const std::wstring index = nextArgument(i);
            demo::g_Options.showLightProbe = index.empty() ? 0 : std::stoi(index);
        }
        else if (lstrcmpW(arg, L"-meshShaders") == 0)
        {
            // Developer option (deviation): selects the mesh-shading path, "nvapi" (the 2018 NVAPI task / mesh
            // shaders) or "d3d12" (the SM 6.5 port). Default: nvapi when the NVAPI check passes, else d3d12.
            meshShadersOption = nextArgument(i);
        }
        else if (lstrcmpW(arg, L"-screenshot") == 0)
        {
            // Developer option (deviation, not in the binary): "-screenshot <file.png> <frame>" saves the
            // scene image of the given rendered frame and closes the window.
            screenshotPath = nextArgument(i);
            const std::wstring frame = nextArgument(i);
            screenshotFrame = frame.empty() ? 1u : uint32_t(std::stoul(frame));
        }
    }
    LocalFree(argv);

    static FILE* s_LogFile = nullptr;
    if (!logPath.empty() && _wfopen_s(&s_LogFile, logPath.c_str(), L"w") == 0 && s_LogFile)
    {
        donut::log::SetCallback([](donut::log::Severity severity, const char* message)
        {
            static const char* const names[] = { "", "DEBUG", "INFO", "WARNING", "ERROR", "FATAL" };
            const int index = std::clamp(int(severity), 0, 5);
            fprintf(s_LogFile, "%s: %s\n", names[index], message);
            fflush(s_LogFile);
            OutputDebugStringA(message);
            OutputDebugStringA("\n");
        });
    }

    // deviation: the binary created DeviceManager_DX12 directly and initialized NVAPI here (0x1400013D0).
    auto deviceManager = nvrhi::TakeOver(DeviceManager::Create(nvrhi::GraphicsAPI::D3D12));

    // deviation: the 2018 parameters carried an adapter name substring; donut selects adapters by index,
    // so the name is matched against the enumerated adapters.
    if (!adapterName.empty() && deviceManager->CreateInstance(g_DeviceParams))
    {
        const std::string needle = WideToUtf8(adapterName);
        std::vector<donut::app::AdapterInfo> adapters;
        if (deviceManager->EnumerateAdapters(adapters))
        {
            for (size_t index = 0; index < adapters.size(); index++)
            {
                if (adapters[index].name.find(needle) != std::string::npos)
                {
                    g_DeviceParams.adapterIndex = int(index);
                    break;
                }
            }
        }
    }

    if (showDialog && DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_STARTUP), GetDesktopWindow(), DialogFunc, 0) != IDOK)
        return 0;

    // A benchmark always plays a replay (replay.0.json unless -replay says otherwise).
    if (demo::g_Options.benchmark)
        demo::g_Options.replayIndex = std::max(demo::g_Options.replayIndex, 0);

    std::stringstream title;
    title << "NVIDIA Asteroids Demo";

    if (!deviceManager->CreateWindowDeviceAndSwapChain(g_DeviceParams, title.str().c_str()))
    {
        MessageBoxA(nullptr, "Cannot initialize the D3D12 device with the requested parameters", "Error", MB_ICONERROR);
        return 1;
    }

    if (HICON icon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ASTEROIDS)))
        SendMessageW(glfwGetWin32Window(deviceManager->GetWindow()), WM_SETICON, ICON_BIG, LPARAM(icon));

    // 2018: NVAPI mesh-shader support query (0x140001980 on the ID3D12Device); the demo ran only if it reported
    // support. deviation: the rebuilt demo also has the D3D12 SM 6.5 mesh-shader path, used when the NVAPI check
    // fails (or with "-meshShaders d3d12") if the device supports D3D12 mesh shaders.
    nvrhi::IDevice* device = deviceManager->GetDevice();
    const bool nvapiMeshShaders = IsNvMeshShaderSupported(device);
    const bool d3d12MeshShaders = device->queryFeatureSupport(nvrhi::Feature::Meshlets);

    bool meshShadersSupported = nvapiMeshShaders || d3d12MeshShaders;
    MeshShaderMode meshShaderMode = nvapiMeshShaders ? MeshShaderMode::Nvapi : MeshShaderMode::D3D12;
    if (!meshShadersOption.empty())
    {
        MeshShaderMode requested;
        if (!ParseMeshShaderMode(WideToUtf8(meshShadersOption), requested))
            donut::log::warning("Unknown -meshShaders value '%s' (expected nvapi or d3d12)", WideToUtf8(meshShadersOption).c_str());
        else
        {
            meshShaderMode = requested;
            meshShadersSupported = (requested == MeshShaderMode::Nvapi) ? nvapiMeshShaders : d3d12MeshShaders;
        }
    }

    if (!meshShadersSupported)
    {
        MessageBoxA(nullptr,
            "The GPU does not support mesh shaders that are required for the demo to run. The demo will now close.",
            "Error", MB_ICONERROR);
        return 1;
    }

    OutputDebugStringA("Meshlets are supported on your hardware\n");
    SetMeshShaderMode(meshShaderMode);
    donut::log::info("Mesh shaders: %s (NVAPI mesh shaders %s, D3D12 mesh shaders %s)",
        GetMeshShaderModeName(meshShaderMode), nvapiMeshShaders ? "supported" : "not supported",
        d3d12MeshShaders ? "supported" : "not supported");

    UIData uiData;
    for (const std::string& assignment : demo::g_Options.uiOverrides)
        ApplyUIOverride(uiData, assignment);
    if (demo::g_Options.showLightProbe >= 0)
    {
        uiData.showLightProbe = true;
        uiData.lightProbeIndex = demo::g_Options.showLightProbe;
    }

    {
        auto demoPass = MAKE_RC_OBJ_PTR(FeatureDemo, deviceManager.Get(), &uiData, asyncLoad);
        auto uiPass = MAKE_RC_OBJ_PTR(UIRenderer, deviceManager.Get(), demoPass, uiData);
        if (!screenshotPath.empty())
            demoPass->RequestScreenshot(screenshotPath, screenshotFrame, screenshotCount);

        donut::vfs::IFileSystem& fs = *demoPass->GetRootFileSystem();
        const std::filesystem::path& mediaPath = demoPass->GetMediaPath();
        uiPass->LoadFont(fs, mediaPath / "OpenSansFont/OpenSans-Regular.ttf", 17.f);
        uiPass->LoadFont(fs, mediaPath / "GeForceFont/geforce-light.ttf", 51.f);
        uiPass->LoadFont(fs, mediaPath / "GeForceFont/geforce-bold.ttf", 51.f);
        uiPass->Init(demoPass->GetShaderFactory());

        deviceManager->AddRenderPassToBack(demoPass.Get());
        deviceManager->AddRenderPassToBack(uiPass.Get());

        deviceManager->RunMessageLoop();

        // Closing the window while the scene still loads leaves the loader thread running: exit hard.
        if (!demoPass->IsSceneLoaded())
            ExitProcess(0);

        // deviation: the binary released the passes after DeviceManager::Shutdown; donut expects the
        // resources to be gone before the device is destroyed.
        deviceManager->RemoveRenderPass(uiPass.Get());
        deviceManager->RemoveRenderPass(demoPass.Get());
    }

    deviceManager->Shutdown();

    return 0;
}

#endif // ASTEROIDS_MAIN_EXE

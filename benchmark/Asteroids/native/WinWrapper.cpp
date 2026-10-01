// Copyright 2014 Intel Corporation All Rights Reserved
//
// Intel makes no representations about the suitability of this software for any purpose.  
// THIS SOFTWARE IS PROVIDED ""AS IS."" INTEL SPECIFICALLY DISCLAIMS ALL WARRANTIES,
// EXPRESS OR IMPLIED, AND ALL LIABILITY, INCLUDING CONSEQUENTIAL AND OTHER INDIRECT DAMAGES,
// FOR THE USE OF THIS SOFTWARE, INCLUDING LIABILITY FOR INFRINGEMENT OF ANY PROPRIETARY
// RIGHTS, AND INCLUDING THE WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
// Intel does not assume any responsibility for any errors which may appear in this software
// nor any responsibility to update it.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN // Exclude rarely-used stuff from Windows headers
#endif
#include <sdkddkver.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h> // must be after windows.h
#include <ShellScalingApi.h>
#include <interactioncontext.h>

// windows.h macros that collide with std:: and project names
#undef GetObject
#undef FindResource
#undef CreateDirectory
#undef DeleteFile
#undef min
#undef max

#include <mmsystem.h>
#include <map>
#include <vector>
#include <iostream>

#include "asteroids_d3d11.h"
#include "asteroids_d3d12.h"
#include "camera.h"
#include "gui.h"
#include "benchmark_support.h"

#include <exception>
#include <string>

using namespace DirectX;

namespace {

// Global demo state
Settings gSettings;
OrbitCamera gCamera;

IDXGIFactory2* gDXGIFactory = nullptr;

AsteroidsD3D11::Asteroids* gWorkloadD3D11 = nullptr;
AsteroidsD3D12::Asteroids* gWorkloadD3D12 = nullptr;
bool gd3d11Available = false;
bool gd3d12Available = false;
Settings::RenderMode gLastFrameRenderMode = static_cast<Settings::RenderMode>(-1);
bool gUpdateWorkload = false;

GUI gGUI;
GUIText* gFPSControl;

enum
{
    basicStyle = WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_VISIBLE,
    windowedStyle = basicStyle | WS_OVERLAPPEDWINDOW,
    fullscreenStyle = basicStyle
};

bool CheckDll(char const* dllName)
{
    auto hModule = LoadLibrary(dllName);
    if (hModule == NULL) {
        return false;
    }
    FreeLibrary(hModule);
    return true;
}

// Benchmark mode (-benchmark): the contract of common/benchmark.h
bool BenchmarkMode() { return Benchmark::config.enabled; }

[[noreturn]] void BenchmarkTerminateHandler()
{
    // ThrowIfFailed() ends up here. Without this handler abort() would
    // exit with code 3, which the contract reserves for "unsupported".
    Benchmark::Fail("unhandled C++ exception or failed HRESULT (std::terminate)");
}

LONG WINAPI BenchmarkUnhandledExceptionFilter(EXCEPTION_POINTERS* info)
{
    char reason[96];
    sprintf_s(reason, "unhandled structured exception 0x%08lx", info != nullptr ? info->ExceptionRecord->ExceptionCode : 0ul);
    Benchmark::Fail(reason);
}

void ResetCameraView()
{
    auto center    = XMVectorSet(0.0f, -0.4f*SIM_DISC_RADIUS, 0.0f, 0.0f);
    auto radius    = SIM_ORBIT_RADIUS + SIM_DISC_RADIUS + 10.f;
    auto minRadius = SIM_ORBIT_RADIUS - 3.0f * SIM_DISC_RADIUS;
    auto maxRadius = SIM_ORBIT_RADIUS + 3.0f * SIM_DISC_RADIUS;
    auto longAngle = 4.50f;
    auto latAngle  = 1.45f;
    gCamera.View(center, radius, minRadius, maxRadius, longAngle, latAngle);
}

void ToggleFullscreen(HWND hWnd)
{
    static WINDOWPLACEMENT prevPlacement = { sizeof(prevPlacement) };
    DWORD dwStyle = (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE);
    if ((dwStyle & windowedStyle) == windowedStyle)
    {
        MONITORINFO mi = { sizeof(mi) };
        if (GetWindowPlacement(hWnd, &prevPlacement) &&
            GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY), &mi))
        {
            SetWindowLong(hWnd, GWL_STYLE, fullscreenStyle);
            SetWindowPos(hWnd, HWND_TOP,
                mi.rcMonitor.left, mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        SetWindowLong(hWnd, GWL_STYLE, windowedStyle);
        SetWindowPlacement(hWnd, &prevPlacement);
        SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}


} // namespace



LRESULT CALLBACK WindowProc(
    HWND hWnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (message) {
        case WM_DESTROY:
            // If all workloads are null, we are recreating the window
            // and should not post quit message
            if(gWorkloadD3D11 || gWorkloadD3D12)
                PostQuitMessage(0);
            return 0;

        case WM_SIZE: {
            UINT ww = LOWORD(lParam);
            UINT wh = HIWORD(lParam);

            gSettings.windowWidth = (int)ww;
            gSettings.windowHeight = (int)wh;
            gSettings.renderWidth = gSettings.windowWidth;
            gSettings.renderHeight = gSettings.windowHeight;

            // Update camera projection
            if(gSettings.renderWidth !=0 && gSettings.renderHeight !=0)
            {
                float aspect = (float)gSettings.renderWidth / (float)gSettings.renderHeight;
                gCamera.Projection(XM_PIDIV2 * 0.8f * 3 / 2, aspect);
            }

            // Resize currently active swap chain
            switch (gSettings.mode)
            {
                case Settings::RenderMode::NativeD3D11: 
                    if(gWorkloadD3D11 && gSettings.renderWidth !=0 && gSettings.renderHeight !=0)
                        gWorkloadD3D11->ResizeSwapChain(gDXGIFactory, hWnd, gSettings.renderWidth, gSettings.renderHeight); 
                break;

                case Settings::RenderMode::NativeD3D12: 
                    if(gWorkloadD3D12 && gSettings.renderWidth !=0 && gSettings.renderHeight !=0)
                        gWorkloadD3D12->ResizeSwapChain(gDXGIFactory, hWnd, gSettings.renderWidth, gSettings.renderHeight); 
                break;

                default:
                break;
            }

            return 0;
        }

        case WM_KEYDOWN:
            if (lParam & (1 << 30)) {
                // Ignore repeats
                return 0;
            }
            if (BenchmarkMode()) {
                // No interaction in benchmark mode: the settings and the camera must not change
                return 0;
            }
            switch (wParam) {
            case VK_SPACE:
                gSettings.animate = !gSettings.animate;
                std::cout << "Animate: " << gSettings.animate << std::endl;
                return 0;

                /* Disabled for demo setup */
            case 'V':
                gSettings.vsync = !gSettings.vsync;
                std::cout << "Vsync: " << gSettings.vsync << std::endl;
                return 0;
            case 'M':
                gSettings.multithreadedRendering = !gSettings.multithreadedRendering;
                std::cout << "Multithreaded Rendering: " << gSettings.multithreadedRendering << std::endl;
                return 0;
            case 'I':
                gSettings.executeIndirect = !gSettings.executeIndirect;
                std::cout << "ExecuteIndirect Rendering: " << gSettings.executeIndirect << std::endl;
                return 0;
            case 'S':
                gSettings.submitRendering = !gSettings.submitRendering;
                std::cout << "Submit Rendering: " << gSettings.submitRendering << std::endl;
                return 0;
            case VK_ADD:
            case VK_OEM_PLUS:
                gSettings.numThreads = std::min(gSettings.numThreads+1, 16);
                gUpdateWorkload = true;
                return 0;
            case VK_SUBTRACT:
            case VK_OEM_MINUS:
                gSettings.numThreads = std::max(gSettings.numThreads-1, 2);
                gUpdateWorkload = true;
                return 0;

            case '1': gSettings.mode = gd3d11Available ? Settings::RenderMode::NativeD3D11 : gSettings.mode; return 0;
            case '2': gSettings.mode = gd3d12Available ? Settings::RenderMode::NativeD3D12 : gSettings.mode; return 0;

            case VK_ESCAPE:
                SendMessage(hWnd, WM_CLOSE, 0, 0);
                return 0;
            } // Switch on key code
            return 0;

        case WM_SYSKEYDOWN:
            if (lParam & (1 << 30)) {
                // Ignore repeats
                return 0;
            }
            if (BenchmarkMode()) {
                return 0;
            }
            switch (wParam) {
            case VK_RETURN:
                gSettings.windowed = !gSettings.windowed;
                ToggleFullscreen(hWnd);
                break;
            }
            return 0;

        case WM_MOUSEWHEEL: {
            if (BenchmarkMode()) {
                return 0;
            }
            auto delta = GET_WHEEL_DELTA_WPARAM(wParam);
            gCamera.ZoomRadius(-0.07f * delta);
            return 0;
        }

        case WM_POINTERDOWN:
        case WM_POINTERUPDATE:
        case WM_POINTERUP: {
            if (BenchmarkMode()) {
                return 0;
            }
            auto pointerId = GET_POINTERID_WPARAM(wParam);
            POINTER_INFO pointerInfo;
            if (GetPointerInfo(pointerId, &pointerInfo)) {
                if (message == WM_POINTERDOWN) {
                    // Compute pointer position in render units
                    POINT p = pointerInfo.ptPixelLocation;
                    ScreenToClient(hWnd, &p);
                    RECT clientRect;
                    GetClientRect(hWnd, &clientRect);
                    p.x = p.x * gSettings.renderWidth / (clientRect.right - clientRect.left);
                    p.y = p.y * gSettings.renderHeight / (clientRect.bottom - clientRect.top);

                    auto guiControl = gGUI.HitTest(p.x, p.y);
                    if (guiControl == gFPSControl) {
                        gSettings.lockFrameRate = !gSettings.lockFrameRate;
                    } else { // Camera manipulation
                        gCamera.AddPointer(pointerId);
                    }
                }

                // Otherwise send it to the camera controls
                gCamera.ProcessPointerFrames(pointerId, &pointerInfo);
                if (message == WM_POINTERUP) gCamera.RemovePointer(pointerId);
            }
            return 0;
        }
                           
        case WM_GETMINMAXINFO:
        {
            LPMINMAXINFO lpMMI = (LPMINMAXINFO)lParam;
            lpMMI->ptMinTrackSize.x = 320;
            lpMMI->ptMinTrackSize.y = 240;
            return 0;
        }

        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
}

int InitWorkload(HWND hWnd, AsteroidsSimulation &asteroids)
{
    switch (gSettings.mode)
    {
        case Settings::RenderMode::NativeD3D11: 
            if (gd3d11Available) {
                gWorkloadD3D11 = new AsteroidsD3D11::Asteroids(&asteroids, &gGUI, gSettings.warp);
            } else if (BenchmarkMode()) {
                Benchmark::Fail("d3d11.dll is not available");
            }

            gWorkloadD3D11->ResizeSwapChain(gDXGIFactory, hWnd, gSettings.renderWidth, gSettings.renderHeight);
        break;

        case Settings::RenderMode::NativeD3D12: 
            if (gd3d12Available) {
                // If requested, enumerate the warp adapter
                // TODO: Allow picking from multiple hardware adapters
                IDXGIAdapter1* adapter = nullptr;

                if (gSettings.warp) {
                    IDXGIFactory4* DXGIFactory4 = nullptr;
                    if (FAILED(gDXGIFactory->QueryInterface(&DXGIFactory4))) {
                        fprintf(stderr, "error: WARP requires IDXGIFactory4 interface which is not present on this system!\n");
                        return -1;
                    }

                    auto hr = DXGIFactory4->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
                    DXGIFactory4->Release();

                    if (FAILED(hr)) {
                        fprintf(stderr, "error: WARP adapter not present on this system!\n");
                        return -1;
                    }
                } else if (Benchmark::config.adapterLuid != 0) {
                    // Benchmark: pin the adapter by LUID (verified in the renderer after device creation)
                    adapter = Benchmark::FindDxgiAdapter(Benchmark::config.adapterLuid);
                    if (adapter == nullptr) {
                        Benchmark::Fail("no DXGI adapter with LUID " + std::to_string(Benchmark::config.adapterLuid));
                    }
                }

                gWorkloadD3D12 = new AsteroidsD3D12::Asteroids(&asteroids, &gGUI, gSettings.numThreads, adapter);
                SafeRelease(&adapter);
            } else if (BenchmarkMode()) {
                Benchmark::Fail("d3d12.dll is not available");
            }

            gWorkloadD3D12->ResizeSwapChain(gDXGIFactory, hWnd, gSettings.renderWidth, gSettings.renderHeight);
        break;

        default:
        break;
    }

    return 0;
}

void CreateDemoWindow(HWND& hWnd)
{
    RECT windowRect = { 100, 100, gSettings.windowWidth, gSettings.windowHeight };
    if (BenchmarkMode()) {
        // Benchmark: the client area (= back buffer) is exactly -window W H
        windowRect.right  = windowRect.left + gSettings.windowWidth;
        windowRect.bottom = windowRect.top + gSettings.windowHeight;
    }
    if(hWnd)
    {
        GetWindowRect(hWnd, &windowRect);
        DestroyWindow(hWnd);
    }
    else
    {
        AdjustWindowRect(&windowRect, windowedStyle, FALSE);
    }

    // create the window and store a handle to it
    hWnd = CreateWindowEx(
        WS_EX_APPWINDOW,
        "AsteroidsDemoWndClass",
        "Asteroids",
        windowedStyle,
        windowRect.left,
        windowRect.top,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        NULL,
        NULL,
        GetModuleHandle(NULL),
        NULL);

    if (!gSettings.windowed) {
        ToggleFullscreen(hWnd);
    }

    SetForegroundWindow(hWnd);
    if (BenchmarkMode())
        Benchmark::KeepWindowOnTop(hWnd); // displayed even when Windows denies the foreground to this process
}

int main(int argc, char** argv)
{
#if defined(_DEBUG) || defined(DEBUG)
    _CrtSetDbgFlag( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
#endif
    
    gd3d11Available = CheckDll("d3d11.dll");
    gd3d12Available = CheckDll("d3d12.dll");

    // Benchmark: pick up -output first so that an argument error can be recorded in DIR/run.json
    for (int a = 1; a + 1 < argc; ++a) {
        if (_stricmp(argv[a], "-output") == 0) {
            Benchmark::config.output = argv[a + 1];
        }
    }

    const char* legacyWorkloadFlag = nullptr; // legacy flag that changes the workload; rejected in benchmark mode
    int nativeGeometryHeap = -1;              // -native_geometry_heap: -1 = not given, 0 = upload, 1 = default
    int nativePresentTearing = -1;            // -native_present: -1 = not given, 0 = default, 1 = tearing
    gSettings.mode = Settings::RenderMode::Undefined;
    for (int a = 1; a < argc; ++a) {
        // Benchmark contract flags (-benchmark, -renderer, -api, -binding, -threads, -warmup, -duration, -window,
        // -adapter_luid, -gpu_timing, -output, -capture_frame). -threads and -window replace the legacy parsing.
        std::string benchmarkError;
        const auto  argResult = Benchmark::ParseArg(argc, argv, a, Benchmark::config, benchmarkError);
        if (argResult == Benchmark::ArgResult::Consumed) {
            continue;
        }
        if (argResult == Benchmark::ArgResult::Error) {
            fprintf(stderr, "error: %s\n%s", benchmarkError.c_str(), Benchmark::Usage());
            Benchmark::Fail(benchmarkError);
        }

        if (_stricmp(argv[a], "-close_after") == 0 && a + 1 < argc) {
            legacyWorkloadFlag = "-close_after";
            gSettings.closeAfterSeconds = atof(argv[++a]);
        } else if (_stricmp(argv[a], "-nod3d11") == 0) {
            gd3d11Available = false;
        } else if (_stricmp(argv[a], "-singlethreaded") == 0) {
            gSettings.multithreadedRendering = false;
        } else if (_stricmp(argv[a], "-warp") == 0) {
            legacyWorkloadFlag = "-warp";
            gSettings.warp = true;
        } else if (_stricmp(argv[a], "-nod3d12") == 0) {
            gd3d12Available = false;
        } else if (_stricmp(argv[a], "-indirect") == 0) {
            legacyWorkloadFlag = "-indirect";
            gSettings.executeIndirect = true;
        } else if (_stricmp(argv[a], "-fullscreen") == 0) {
            legacyWorkloadFlag = "-fullscreen";
            gSettings.windowed = false;
        } else if (_stricmp(argv[a], "-locked_fps") == 0 && a + 1 < argc) {
            gSettings.lockedFrameRate = atoi(argv[++a]);
        } else if (_stricmp(argv[a], "-native_geometry_heap") == 0 && a + 1 < argc) {
            // Native D3D12 static geometry: upload (stock sample) or default heap
            const char* value = argv[++a];
            if (_stricmp(value, "default") == 0) {
                nativeGeometryHeap = 1;
            } else if (_stricmp(value, "upload") == 0) {
                nativeGeometryHeap = 0;
            } else {
                Benchmark::Fail(std::string("invalid value '") + value + "' for -native_geometry_heap (upload, default)");
            }
        } else if (_stricmp(argv[a], "-native_present") == 0 && a + 1 < argc) {
            // Native D3D11/D3D12 swap chain: tearing (ALLOW_TEARING, really unsynchronized) or default (stock sample)
            const char* value = argv[++a];
            if (_stricmp(value, "tearing") == 0) {
                nativePresentTearing = 1;
            } else if (_stricmp(value, "default") == 0) {
                nativePresentTearing = 0;
            } else {
                Benchmark::Fail(std::string("invalid value '") + value + "' for -native_present (tearing, default)");
            }
        } else if (_stricmp(argv[a], "-d3d11") == 0) {
            gSettings.mode = Settings::RenderMode::NativeD3D11;
        } else if (_stricmp(argv[a], "-d3d12") == 0) {
            gSettings.mode = gd3d12Available ? Settings::RenderMode::NativeD3D12 : Settings::RenderMode::Undefined;
        } else {
            fprintf(stderr, "error: unrecognized argument '%s'\n", argv[a]);
            fprintf(stderr, "usage: asteroids_d3d12 [options]\n");
            fprintf(stderr, "options:\n");
            fprintf(stderr, "  -close_after [seconds]\n");
            fprintf(stderr, "  -nod3d11\n");
            fprintf(stderr, "  -nod3d12\n");
            fprintf(stderr, "  -fullscreen\n");
            fprintf(stderr, "  -window [width] [height]\n");
            fprintf(stderr, "  -render_scale [scale]\n");
            fprintf(stderr, "  -locked_fps [fps]\n");
            fprintf(stderr, "  -warp\n");
            fprintf(stderr, "  -singlethreaded\n");
            fprintf(stderr, "  -indirect\n");
            fprintf(stderr, "  -threads [count]\n");
            fprintf(stderr, "  -d3d11 | -d3d12\n");
            fprintf(stderr, "  -native_geometry_heap upload|default   (native D3D12 static geometry)\n");
            fprintf(stderr, "  -native_present tearing|default        (native D3D11/D3D12 swap chain)\n");
            fprintf(stderr, "benchmark options:\n%s", Benchmark::Usage());
            if (BenchmarkMode()) {
                Benchmark::Fail(std::string("unrecognized argument '") + argv[a] + "'");
            }
            return -1;
        }
    }

    // -threads and -window were consumed by the benchmark parser
    if (Benchmark::config.hasThreads) {
        gSettings.numThreads = Benchmark::config.threads;
    }
    if (Benchmark::config.hasWindow) {
        gSettings.windowWidth = Benchmark::config.width;
        gSettings.windowHeight = Benchmark::config.height;
    }

    // Options of the native renderers. Outside benchmark mode the stock sample's behaviour is the default.
    // Benchmark mode defaults (measured on the RTX 4050, see docs/LIMITATIONS.md):
    //  - static geometry in a DEFAULT heap: reading it from the UPLOAD heap doubles the GPU time of a frame
    //    (2.4 ms instead of 1.1 ms) and makes native D3D12 GPU-bound above 1 thread;
    //  - tearing presentation: without ALLOW_TEARING the windowed flip-model swap chain can be paced by the
    //    compositor (observed: 240 frames per second on a 120 Hz display) although the sync interval is 0.
    //    Donut requests ALLOW_TEARING whenever it is supported.
    BenchSupport::options.nativeGeometryDefaultHeap = nativeGeometryHeap == 1 || (nativeGeometryHeap < 0 && BenchmarkMode());
    if (nativePresentTearing == 1 && !BenchSupport::TearingSupported()) {
        Benchmark::Fail("-native_present tearing: DXGI_FEATURE_PRESENT_ALLOW_TEARING is not supported");
    }
    BenchSupport::options.nativeAllowTearing =
        nativePresentTearing == 1 || (nativePresentTearing < 0 && BenchmarkMode() && BenchSupport::TearingSupported());

    if (BenchmarkMode()) {
        const auto& config = Benchmark::config;

        static const char* const renderers[] = {"native"};
        Benchmark::ValidateOrFail(config, renderers, 1); // exits on an invalid or unsupported combination

        if (legacyWorkloadFlag != nullptr) {
            Benchmark::Fail(std::string(legacyWorkloadFlag) + " cannot be combined with -benchmark");
        }

        // From here on every failure must leave a failed run.json and a contract exit code
        std::set_terminate(BenchmarkTerminateHandler);
        SetUnhandledExceptionFilter(BenchmarkUnhandledExceptionFilter);

        // -renderer / -api -> render mode
        // -binding is ignored by the native renderers (one fixed strategy, recorded as "original")
        if (config.api == "d3d11") {
            if (!gd3d11Available) Benchmark::Fail("d3d11.dll is not available");
            gSettings.mode = Settings::RenderMode::NativeD3D11;
        } else if (config.api == "d3d12") {
            if (!gd3d12Available) Benchmark::Fail("d3d12.dll is not available");
            gSettings.mode = Settings::RenderMode::NativeD3D12;
        } else {
            Benchmark::Fail("the native renderers have no " + config.api + " backend", Benchmark::FailKind::Unsupported);
        }

        // -threads N: N command-recording threads, 1 = single-threaded rendering
        if (gSettings.mode == Settings::RenderMode::NativeD3D11 && config.threads != 1) {
            Benchmark::Fail("the native D3D11 renderer is single-threaded (-threads 1 only)", Benchmark::FailKind::Unsupported);
        }
        gSettings.numThreads = config.threads;
        gSettings.multithreadedRendering = config.threads > 1;

        gSettings.windowWidth = config.width;
        gSettings.windowHeight = config.height;
        gSettings.windowed = true;
        gSettings.vsync = false;
        gSettings.lockFrameRate = false;
        gSettings.animate = true;
        gSettings.submitRendering = true;
        gSettings.executeIndirect = false;
        gSettings.warp = false;
        gSettings.closeAfterSeconds = 0.0;

        // The back buffer must have exactly the requested size in physical pixels
        SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
    }
    
    if (gSettings.numThreads == 0)
    {
        gSettings.numThreads = std::max(std::thread::hardware_concurrency()-1, 2u);
    }

    //if (!d3d11Available && !d3d12Available) {
    //    fprintf(stderr, "error: neither D3D11 nor D3D12 available.\n");
    //    return -1;
    //}

    // DXGI Factory
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&gDXGIFactory)));

    // Setup GUI
    gFPSControl = gGUI.AddText(150, 10);

    ResetCameraView();
    // Camera projection set up in WM_SIZE

    AsteroidsSimulation asteroids(Benchmark::kSeed, NUM_ASTEROIDS, NUM_UNIQUE_MESHES, MESH_MAX_SUBDIV_LEVELS, NUM_UNIQUE_TEXTURES);

    if (gSettings.mode == Settings::RenderMode::Undefined)
    {
        gSettings.mode = gd3d12Available ? Settings::RenderMode::NativeD3D12 : Settings::RenderMode::NativeD3D11;
    }

    // init window class
    WNDCLASSEX windowClass;
    ZeroMemory(&windowClass, sizeof(WNDCLASSEX));
    windowClass.cbSize = sizeof(WNDCLASSEX);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = GetModuleHandle(NULL);
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass.lpszClassName = "AsteroidsDemoWndClass";
    RegisterClassEx(&windowClass);

    HWND hWnd = NULL;

    // Initialize performance counters
    UINT64 perfCounterFreq = 0;
    UINT64 lastPerfCount = 0;
    QueryPerformanceFrequency((LARGE_INTEGER*)&perfCounterFreq);
    QueryPerformanceCounter((LARGE_INTEGER*)&lastPerfCount);

    // main loop
    double elapsedTime = 0.0;
    double frameTime = 0.0;
    POINTER_INFO pointerInfo = {};

    timeBeginPeriod(1);
    EnableMouseInPointer(TRUE);

    float filteredUpdateTime = 0.0f;
    float filteredRenderTime = 0.0f;
    float filteredFrameTime = 0.0f;
    for (;;)
    {
        MSG msg = {};
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                if (BenchmarkMode()) {
                    Benchmark::Fail("the window was closed before the benchmark finished");
                }
                // Cleanup
                delete gWorkloadD3D11;
                delete gWorkloadD3D12;
                SafeRelease(&gDXGIFactory);
                timeEndPeriod(1);
                EnableMouseInPointer(FALSE);
                return (int)msg.wParam;
            };

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // If we swap to a new API we need to recreate swap chains
        if (gLastFrameRenderMode != gSettings.mode || gUpdateWorkload) {
            // Delete workload first so that the window does not
            // post quit message to the queue
            delete gWorkloadD3D11;
            gWorkloadD3D11 = nullptr;
            delete gWorkloadD3D12;
            gWorkloadD3D12 = nullptr;
            if (hWnd == NULL || gLastFrameRenderMode != gSettings.mode)
                CreateDemoWindow(hWnd);
            InitWorkload(hWnd, asteroids);
            gLastFrameRenderMode = gSettings.mode;
            gUpdateWorkload = false;
        }

        // Benchmark: open the frame. The first call starts the warmup clock (after the workload was created);
        // false means the measured duration has elapsed.
        if (BenchmarkMode() && !Benchmark::recorder.BeginFrame()) {
            break;
        }

        // Still need to process inertia even when no interaction is happening
        if (!BenchmarkMode()) {
            gCamera.ProcessInertia();
        }

        // In D3D12 we'll wait on the GPU before taking the timestamp (more consistent)
        if (gSettings.mode == Settings::RenderMode::NativeD3D12) {
            Benchmark::ScopedTimer waitTimer(Benchmark::recorder.Times().wait);
            gWorkloadD3D12->WaitForReadyToRender();
        }

        // Get time delta
        UINT64 count;
        QueryPerformanceCounter((LARGE_INTEGER*)&count);
        auto rawFrameTime = (double)(count - lastPerfCount) / perfCounterFreq;
        elapsedTime += rawFrameTime;
        lastPerfCount = count;

        // Maintaining absolute time sync is not important in this demo so we can err on the "smoother" side
        double alpha = 0.2f;
        frameTime = alpha * rawFrameTime + (1.0f - alpha) * frameTime;

        // Benchmark: fixed simulation timestep instead of the smoothed wall-clock delta
        const float simFrameTime = Benchmark::FrameDelta((float)frameTime);

        // Update GUI (off in benchmark mode: no window title updates, no FPS text)
        if (!BenchmarkMode())
        {
            const char *ModeStr = nullptr;
            float updateTime = 0;
            float renderTime = 0;
            switch (gSettings.mode)
            {
                case Settings::RenderMode::NativeD3D11: 
                    ModeStr = "Native D3D11";
                    gWorkloadD3D11->GetPerfCounters(updateTime, renderTime);
                break;

                case Settings::RenderMode::NativeD3D12: 
                    ModeStr = "Native D3D12";
                    gWorkloadD3D12->GetPerfCounters(updateTime, renderTime);
                break;

                default:
                break;
            }

            float filterScale = 0.02f;
            filteredUpdateTime = filteredUpdateTime * (1.f - filterScale) + filterScale * updateTime;
            filteredRenderTime = filteredRenderTime * (1.f - filterScale) + filterScale * renderTime;
            filteredFrameTime = filteredFrameTime * (1.f - filterScale) + filterScale * (float)frameTime;

            char buffer[256];
            sprintf_s(buffer, "Asteroids %s (%dt) - %4.1f ms (%4.1f ms / %4.1f ms)", ModeStr, (gSettings.multithreadedRendering ? gSettings.numThreads : 1), 
                              1000.f * filteredFrameTime, 1000.f * filteredUpdateTime, 1000.f * filteredRenderTime);

            SetWindowText(hWnd, buffer);

            if (gSettings.lockFrameRate) {
                sprintf_s(buffer, "(Locked)");
            } else {
                sprintf_s(buffer, "%.0f fps", 1.0f / filteredFrameTime);
            }
            gFPSControl->Text(buffer);
        }

        switch (gSettings.mode)
        {
            case Settings::RenderMode::NativeD3D11: 
                if(gWorkloadD3D11)
                    gWorkloadD3D11->Render(simFrameTime, gCamera, gSettings);
            break;

            case Settings::RenderMode::NativeD3D12: 
                if(gWorkloadD3D12)
                    gWorkloadD3D12->Render(simFrameTime, gCamera, gSettings);
            break;

            default:
            break;
        }

        if (gSettings.lockFrameRate) {

            UINT64 afterRenderCount;
            QueryPerformanceCounter((LARGE_INTEGER*)&afterRenderCount);
            double renderTime = (double)(afterRenderCount - count) / perfCounterFreq;

            double targetRenderTime = 1.0 / double(gSettings.lockedFrameRate);
            double deltaMs = (targetRenderTime - renderTime) * 1000.0;
            if (deltaMs > 1.0) {
                Sleep((DWORD)deltaMs);
            }

        }

        // All done?
        if (gSettings.closeAfterSeconds > 0.0 && elapsedTime > gSettings.closeAfterSeconds) {
            SendMessage(hWnd, WM_CLOSE, 0, 0);
            break;
        }
    }

    if (BenchmarkMode()) {
        // The measured duration has elapsed: collect the run facts, write frames.csv and run.json
        Benchmark::RunInfo info;
        info.asteroids = NUM_ASTEROIDS;
        info.meshes = NUM_UNIQUE_MESHES;
        info.textures = NUM_UNIQUE_TEXTURES;
        info.staticSceneHash = Benchmark::StaticSceneHash(asteroids, (unsigned)NUM_ASTEROIDS);
        info.finalDynamicSceneHash = Benchmark::DynamicSceneHash(asteroids, (unsigned)NUM_ASTEROIDS);
        if (gWorkloadD3D11) {
            gWorkloadD3D11->GetBenchmarkInfo(info);
        } else if (gWorkloadD3D12) {
            gWorkloadD3D12->GetBenchmarkInfo(info, gSettings);
        }
        info.extra.emplace_back("draw_count_definition", "asteroid draws + 1 skybox draw");
        info.extra.emplace_back("index_count_definition", "sum of the index counts of the asteroid draws");
        info.extra.emplace_back("gui", "off");

        if (info.width != Benchmark::config.width || info.height != Benchmark::config.height) {
            Benchmark::Fail("back buffer is " + std::to_string(info.width) + "x" + std::to_string(info.height) +
                            ", requested " + std::to_string(Benchmark::config.width) + "x" + std::to_string(Benchmark::config.height));
        }

        Benchmark::AddDesktopFacts(info, hWnd);

        const int exitCode = Benchmark::Finish(info);

        // Cleanup. Workloads are deleted first so that WM_DESTROY does not post a quit message.
        delete gWorkloadD3D11;
        gWorkloadD3D11 = nullptr;
        delete gWorkloadD3D12;
        gWorkloadD3D12 = nullptr;
        SafeRelease(&gDXGIFactory);
        timeEndPeriod(1);
        EnableMouseInPointer(FALSE);
        return exitCode;
    }

    
    // Shouldn't get here
    return 1;
}

// mt_rendering_native: the MultithreadedRendering sample's D3D12 renderer under the benchmark contract
// (common/benchmark.h). Interactive without -benchmark: left mouse orbits, wheel zooms, keys 1/2/3
// pick the scheduling mode, M toggles the tight mirror scissor, Escape quits.

#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellscalingapi.h>
#include <mmsystem.h>

#include <cstdio>
#include <exception>
#include <memory>
#include <string>

#include "benchmark.h"
#include "renderer_d3d12.h"
#include "scene.h"
#include "sdkmesh.h"

using namespace MtScene;

namespace
{

struct App
{
    Scene scene;
    std::unique_ptr<MtNative::RendererD3D12> renderer;
    bool dragging = false;
    POINT lastMouse = {};
    bool tightScissor = false;
    uint32_t width = 0, height = 0;
    bool resized = false;
};

App* g_App = nullptr;

bool BenchmarkMode() { return Benchmark::config.enabled; }

[[noreturn]] void TerminateHandler()
{
    Benchmark::Fail("unhandled C++ exception (std::terminate)");
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* info)
{
    char reason[96];
    sprintf_s(reason, "unhandled structured exception 0x%08lx", info ? info->ExceptionRecord->ExceptionCode : 0ul);
    Benchmark::Fail(reason);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    App* app = g_App;
    switch (message)
    {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            if (app && wParam != SIZE_MINIMIZED)
            {
                app->width = LOWORD(lParam);
                app->height = HIWORD(lParam);
                app->resized = true;
            }
            return 0;
        case WM_KEYDOWN:
            if (BenchmarkMode() || !app) return 0;
            switch (wParam)
            {
                case VK_ESCAPE: PostMessage(hwnd, WM_CLOSE, 0, 0); return 0;
                case '1': if (app->renderer->Threads() == 1) app->renderer->SetMode(Mode::SingleThreaded); return 0;
                case '2': if (app->renderer->Threads() > 1) app->renderer->SetMode(Mode::PerScene); return 0;
                case '3': if (app->renderer->Threads() > 1) app->renderer->SetMode(Mode::PerChunk); return 0;
                case 'M':
                    app->tightScissor = !app->tightScissor;
                    app->scene.SetTightMirrorScissor(app->tightScissor);
                    return 0;
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (BenchmarkMode() || !app) return 0;
            app->dragging = true;
            app->lastMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            SetCapture(hwnd);
            return 0;
        case WM_LBUTTONUP:
            if (app) app->dragging = false;
            ReleaseCapture();
            return 0;
        case WM_MOUSEMOVE:
            if (app && app->dragging && !BenchmarkMode())
            {
                const POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                app->scene.Orbit(float(p.x - app->lastMouse.x) * 0.005f, float(p.y - app->lastMouse.y) * 0.005f);
                app->lastMouse = p;
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (app && !BenchmarkMode())
                app->scene.Zoom(-0.1f * float(GET_WHEEL_DELTA_WPARAM(wParam)));
            return 0;
        case WM_GETMINMAXINFO:
        {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 320;
            mmi->ptMinTrackSize.y = 240;
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

void PrintUsage()
{
    fprintf(stderr, "usage: mt_rendering_native [options]\n%s"
                    "  -mirror_scissor            tight stencil scissor around each mirror (the sample's option)\n"
                    "  -debug                     D3D12 debug layer\n"
                    "  -media DIR                 directory holding SquidRoom/SquidRoom.sdkmesh (default: <exe dir>/../media)\n"
                    "  -freeze T                  render the scripted frame at time T seconds (parity screenshots)\n"
                    "  -screenshot FILE.bmp       save the 30th frame as a BMP and exit\n",
            Benchmark::Usage());
}

std::filesystem::path ExeDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

} // namespace

int main(int argc, char** argv)
{
    Benchmark::Config& config = Benchmark::config;
    bool debugLayer = false, tightScissor = false;
    double freezeTime = -1.0; // -freeze T: render the scripted frame at time T (parity screenshots)
    std::filesystem::path screenshot; // -screenshot FILE.bmp: save frame 30 and exit
    std::filesystem::path mediaDir = ExeDirectory().parent_path() / "media";

    for (int a = 1; a + 1 < argc; ++a)
        if (_stricmp(argv[a], "-output") == 0) config.output = argv[a + 1];

    for (int a = 1; a < argc; ++a)
    {
        std::string error;
        const Benchmark::ArgResult result = Benchmark::ParseArg(argc, argv, a, config, error);
        if (result == Benchmark::ArgResult::Consumed) continue;
        if (result == Benchmark::ArgResult::Error) { PrintUsage(); Benchmark::Fail(error); }
        if (_stricmp(argv[a], "-debug") == 0) debugLayer = true;
        else if (_stricmp(argv[a], "-mirror_scissor") == 0) tightScissor = true;
        else if (_stricmp(argv[a], "-media") == 0 && a + 1 < argc) mediaDir = argv[++a];
        else if (_stricmp(argv[a], "-freeze") == 0 && a + 1 < argc) freezeTime = atof(argv[++a]);
        else if (_stricmp(argv[a], "-screenshot") == 0 && a + 1 < argc) screenshot = argv[++a];
        else if (_stricmp(argv[a], "-help") == 0 || _stricmp(argv[a], "-h") == 0 || _stricmp(argv[a], "-?") == 0) { PrintUsage(); return 0; }
        else { PrintUsage(); Benchmark::Fail(std::string("unrecognized argument '") + argv[a] + "'"); }
    }

    if (!config.enabled)
    {
        if (config.renderer.empty()) config.renderer = "native";
        if (config.api.empty()) config.api = "d3d12";
        if (config.mode.empty()) config.mode = config.threads > 1 ? "mt_chunk" : "st";
    }
    static const char* const kRenderers[] = { "native" };
    Benchmark::ValidateOrFail(config, kRenderers, 1);
    if (config.renderer != "native") Benchmark::Fail("this executable only implements -renderer native", Benchmark::FailKind::Unsupported);
    if (config.api != "d3d12") Benchmark::Fail("the native renderer is D3D12 only", Benchmark::FailKind::Unsupported);
    {
        std::string reason;
        if (!Benchmark::CombinationSupported(config.renderer, config.api, config.mode, config.threads, reason))
            Benchmark::Fail(reason, Benchmark::FailKind::Unsupported);
    }
    Mode mode;
    ParseMode(config.mode, mode);

    if (config.enabled)
    {
        std::set_terminate(TerminateHandler);
        SetUnhandledExceptionFilter(UnhandledFilter);
    }
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE); // the client area is exactly -window W H pixels
    timeBeginPeriod(1);

    // ---- scene ----
    SdkMesh mesh;
    {
        std::string error;
        if (!mesh.Load(mediaDir / kMeshFile, error))
            Benchmark::Fail(error);
    }
    const uint64_t sceneHash = mesh.ContentHash();
    printf("scene_hash %s (%u meshes, %u materials)\n", Benchmark::Hex64(sceneHash).c_str(), mesh.NumMeshes(), mesh.NumMaterials());

    App app;
    g_App = &app;
    app.tightScissor = tightScissor;
    app.scene.SetTightMirrorScissor(tightScissor);

    // ---- window ----
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.lpszClassName = L"MtRenderingNativeWndClass";
    RegisterClassExW(&wc);

    RECT rect = { 100, 100, 100 + config.width, 100 + config.height };
    const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    AdjustWindowRect(&rect, style, FALSE);
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"MultithreadedRendering (native D3D12)", style, rect.left, rect.top,
                                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) Benchmark::Fail("CreateWindowEx failed");
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    if (config.enabled) Benchmark::KeepWindowOnTop(hwnd);
    app.width = config.width;
    app.height = config.height;

    // ---- renderer ----
    MtNative::RendererDesc desc;
    desc.mode = mode;
    desc.threads = uint32_t(config.threads);
    desc.window = hwnd;
    desc.width = config.width;
    desc.height = config.height;
    desc.debugLayer = debugLayer;
    desc.allowTearing = true;
    {
        IDXGIFactory5* factory = nullptr;
        BOOL supported = FALSE;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        {
            if (FAILED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported, sizeof(supported)))) supported = FALSE;
            factory->Release();
        }
        desc.allowTearing = supported != FALSE;
    }
    IDXGIAdapter1* adapter = nullptr;
    if (config.adapterLuid != 0)
    {
        adapter = Benchmark::FindDxgiAdapter(config.adapterLuid);
        if (!adapter) Benchmark::Fail("no DXGI adapter with LUID " + std::to_string(config.adapterLuid));
    }
    desc.adapter = adapter;
    try
    {
        app.renderer = std::make_unique<MtNative::RendererD3D12>(desc, mesh);
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    if (adapter) adapter->Release();
    app.scene.SetViewport(config.width, config.height);
    printf("adapter %s (LUID %llu)\n", Benchmark::state.adapterName.c_str(), static_cast<unsigned long long>(Benchmark::state.adapterLuid));

    // ---- frame loop ----
    // Benchmark mode: the scene time is the wall clock since the first frame, so that every renderer sees the
    // same camera path and lights over the same measured window regardless of its frame rate.
    const int64_t startStamp = Benchmark::Now();
    int64_t lastStamp = startStamp;
    double time = 0.0;
    double smoothedFrame = 0.0, smoothedRender = 0.0, smoothedSubmit = 0.0;
    int exitCode = 0;
    bool quit = false;
    while (!quit)
    {
        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                if (config.enabled) Benchmark::Fail("the window was closed before the benchmark finished");
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;

        if (app.resized)
        {
            app.resized = false;
            if (app.width && app.height)
            {
                app.renderer->Resize(app.width, app.height);
                app.scene.SetViewport(app.width, app.height);
            }
        }
        if (app.width == 0 || app.height == 0)
        {
            Sleep(10);
            continue;
        }

        if (!Benchmark::recorder.BeginFrame())
            break;

        const int64_t now = Benchmark::Now();
        if (config.enabled) time = Benchmark::Seconds(now - startStamp);
        else time += Benchmark::Seconds(now - lastStamp);
        lastStamp = now;

        Benchmark::FrameTimes& times = Benchmark::recorder.Times();
        {
            Benchmark::ScopedTimer update(times.update);
            if (freezeTime >= 0.0) app.scene.Animate(float(freezeTime), true);
            else app.scene.Animate(float(time), config.enabled);
        }
        app.renderer->RenderFrame(app.scene, times);
        Benchmark::recorder.SetFrameStats(app.renderer->LastFrameDraws(), app.renderer->LastFrameIndices());
        if (!screenshot.empty() && Benchmark::recorder.FrameIndex() == 30)
        {
            std::string error;
            if (!app.renderer->SaveBackBuffer(screenshot, error)) { fprintf(stderr, "error: %s\n", error.c_str()); exitCode = 1; }
            else printf("screenshot -> %s\n", screenshot.string().c_str());
            break;
        }

        if (!config.enabled)
        {
            const double alpha = 0.05;
            smoothedFrame += alpha * (Benchmark::Seconds(Benchmark::Now() - now) * 1000.0 - smoothedFrame);
            smoothedRender += alpha * (times.render - smoothedRender);
            smoothedSubmit += alpha * (times.submit - smoothedSubmit);
            char title[256];
            sprintf_s(title, "MultithreadedRendering native D3D12 | %s | %u threads | render %.2f ms  submit %.2f ms  frame %.2f ms | %llu draws%s",
                      ModeName(app.renderer->GetMode()), app.renderer->Threads(), smoothedRender, smoothedSubmit, smoothedFrame,
                      static_cast<unsigned long long>(app.renderer->LastFrameDraws()), app.tightScissor ? " | tight scissor" : "");
            SetWindowTextA(hwnd, title);
        }
    }

    if (config.enabled)
    {
        Benchmark::RunInfo info;
        info.width = int(app.width);
        info.height = int(app.height);
        info.threads = int(app.renderer->Threads());
        info.meshes = mesh.NumMeshes();
        uint64_t draws = 0, indices = 0;
        mesh.CountDraws(draws, indices);
        info.subsets = draws;
        info.textures = app.renderer->TextureCount();
        info.passes = kNumPasses;
        info.sceneHash = sceneHash;
        info.extra.emplace_back("threading", config.mode == "st" ? "main thread, one command list" :
                                             config.mode == "mt_scene" ? "worker pool: one list per shadow map and mirror, direct view on the main thread, one ExecuteCommandLists" :
                                             "worker pool: every pass split into per-thread chunks, one ExecuteCommandLists per pass");
        info.extra.emplace_back("constant_buffers", "root CBVs from a per-frame upload stack (shared by all threads, one lock per push)");
        info.extra.emplace_back("textures_binding", "two root descriptor tables per subset (diffuse, normal), one shader-visible heap");
        info.extra.emplace_back("mirror_scissor", tightScissor ? "tight" : "full");
        info.extra.emplace_back("swap_chain", desc.allowTearing ? "flip-discard, 3 buffers, ALLOW_TEARING" : "flip-discard, 3 buffers");
        info.extra.emplace_back("draw_count_definition", "sdkmesh subset draws of every pass (shadow, 4 mirrors, direct); the mirror quads are not counted");
        Benchmark::AddDesktopFacts(info, hwnd);
        exitCode = Benchmark::Finish(info);
    }

    app.renderer.reset();
    g_App = nullptr;
    DestroyWindow(hwnd);
    timeEndPeriod(1);
    return exitCode;
}

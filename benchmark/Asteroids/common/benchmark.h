#pragma once
// Shared benchmark contract for asteroids_native (native D3D11/D3D12) and asteroids_nvrhi (Donut + nvrhi,
// D3D11/D3D12/Vulkan). Single source of truth for docs/PLAN.md "Benchmark contract".
//
// Dependencies: C++17, <windows.h>, <dxgi.h> (link dxgi.lib). No D3D11/D3D12/Vulkan/nvrhi headers.
// API-specific helpers live in separate headers: benchmark_d3d11.h, benchmark_d3d12.h.
//
// ---------------------------------------------------------------------------------------------
// Host integration (one translation unit owns main(); every TU may include this header, all
// state is `inline`):
//
//   std::string err;
//   for (int a = 1; a < argc; ++a) {
//       switch (Benchmark::ParseArg(argc, argv, a, Benchmark::config, err)) {
//           case Benchmark::ArgResult::Consumed: continue;
//           case Benchmark::ArgResult::Error:    Benchmark::Fail(err);
//           case Benchmark::ArgResult::NotMine:  /* host-specific flags */ break;
//       }
//   }
//   static const char* const mine[] = {"native"};                      // or {"nvrhi"}
//   Benchmark::ValidateOrFail(Benchmark::config, mine, 1);             // exits on invalid/unsupported
//   ... create window (config.width x config.height client area), device on config.adapterLuid ...
//   Benchmark::SetActualAdapterOrFail(luidOfCreatedDevice);            // LUID check (mandatory with -adapter_luid)
//
//   for (;;) {                                                         // main thread
//       if (!Benchmark::recorder.BeginFrame()) break;                  // false once the measured duration elapsed
//       { Benchmark::ScopedTimer t(Benchmark::recorder.Times().update);  sim.Update(Benchmark::kTimestepF, ...); }
//       { Benchmark::ScopedTimer t(Benchmark::recorder.Times().render);  /* record all command lists, join workers */ }
//       { Benchmark::ScopedTimer t(Benchmark::recorder.Times().submit);  /* close + execute/submit */ }
//       { Benchmark::ScopedTimer t(Benchmark::recorder.Times().present); /* Present */ }
//       { Benchmark::ScopedTimer t(Benchmark::recorder.Times().wait);    /* fence waits, frame pacing */ }
//       Benchmark::recorder.Times().gc += ms;                          // optional: the garbage-collection part of wait
//       Benchmark::recorder.SetFrameStats(drawCount, indexCount);
//   }
//   Benchmark::RunInfo info; ... fill ...
//   return Benchmark::Finish(info);                                    // writes frames.csv + run.json, 0 on success
//
// Columns may be accumulated in any order and several times per frame (each ScopedTimer adds).
// All columns are main-thread wall-clock time. frame_ms is BeginFrame-to-BeginFrame.
// A combination a renderer cannot express must call Benchmark::Fail(reason, Benchmark::FailKind::Unsupported);
// it must never fall back to another mode.
// ---------------------------------------------------------------------------------------------

#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Benchmark
{

// ------------------------------------------------------------------ constants

constexpr unsigned kSeed          = 1337;        // AsteroidsSimulation rngSeed
constexpr double   kTimestep      = 1.0 / 60.0;  // fixed simulation step in benchmark mode
constexpr float    kTimestepF     = 1.0f / 60.0f;
constexpr int      kDefaultWidth  = 1080;
constexpr int      kDefaultHeight = 720;

constexpr int kExitOk          = 0;
constexpr int kExitFailed      = 2; // invalid arguments or runtime error; run.json has "failure_kind":"error"
constexpr int kExitUnsupported = 3; // combination not expressible;       run.json has "failure_kind":"unsupported"

constexpr const char* kRenderers[] = {"native", "nvrhi"};
constexpr const char* kApis[]      = {"d3d11", "d3d12", "vk"};
constexpr const char* kBindings[]  = {"dyn", "mut", "tex_mut", "tex_mut_pc", "bindless"};

constexpr const char* kCsvHeader =
    "frame,update_ms,render_ms,submit_ms,wait_ms,present_ms,frame_ms,gpu_ms,draw_count,index_count,gc_ms";

// ------------------------------------------------------------------ configuration

struct Config
{
    bool        enabled   = false; // -benchmark
    std::string renderer;          // -renderer native|nvrhi
    std::string api;               // -api d3d11|d3d12|vk
    std::string binding;           // -binding dyn|mut|tex_mut|tex_mut_pc|bindless (ignored for native)
    int         threads   = 1;     // -threads N (1 = single-threaded rendering)
    double      warmup    = 10.0;  // -warmup SECONDS
    double      duration  = 30.0;  // -duration SECONDS
    int         width     = kDefaultWidth;  // -window W H (client area)
    int         height    = kDefaultHeight;
    uint64_t    adapterLuid = 0;   // -adapter_luid N (decimal (HighPart << 32) | LowPart; 0 = not pinned)
    bool        gpuTiming = false; // -gpu_timing
    std::string output;            // -output DIR

    // Extension for the parity checks of the integration phase (not used in measured runs):
    // -capture_frame N : the renderer reads back the back buffer of absolute frame N (0-based, counted
    // from the first BeginFrame, warmup included) and passes it to WriteCapture -> DIR/capture.bmp.
    int64_t captureFrame = -1;

    // Which flags were given explicitly.
    bool hasRenderer = false, hasApi = false, hasBinding = false, hasThreads = false, hasWindow = false;
};

inline Config config;

// The binding recorded in run.json: native renderers have one fixed strategy.
inline std::string EffectiveBinding(const Config& c) { return c.renderer == "native" ? "original" : c.binding; }

// ------------------------------------------------------------------ timers (QPC)

inline int64_t Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
inline int64_t Frequency()
{
    static const int64_t f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return q.QuadPart; }();
    return f;
}
inline double Ms(int64_t ticks) { return 1000.0 * double(ticks) / double(Frequency()); }
inline double Seconds(int64_t ticks) { return double(ticks) / double(Frequency()); }

// Adds the elapsed wall-clock milliseconds to `accumulator` on destruction. Main thread only.
class ScopedTimer
{
    double& m_Acc;
    int64_t m_Start;

public:
    explicit ScopedTimer(double& accumulator) : m_Acc(accumulator), m_Start(Now()) {}
    ~ScopedTimer() { m_Acc += Ms(Now() - m_Start); }
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
};

// ------------------------------------------------------------------ per-frame data

struct FrameTimes
{
    double update  = 0; // simulation update
    double render  = 0; // start of command recording until every command list is recorded (workers joined)
    double submit  = 0; // closing and executing/submitting the command lists
    double wait    = 0; // blocking on GPU fences / frame pacing
    double present = 0; // the Present call
    double gpu     = std::numeric_limits<double>::quiet_NaN(); // optional GPU time of this frame
    // The part of `wait` that is the abstraction layer's per-frame garbage collection (deferred release of
    // what the finished command lists referenced). Only the nvrhi renderer has such a separate step (Donut
    // calls IDevice::runGarbageCollection once per frame); 0 for the other renderers, whose equivalent work
    // runs inside Present. Used by the sensitivity metric render + submit + gc of scripts/analyze.py.
    double gc      = 0;
};

struct Row
{
    uint64_t   frame = 0; // absolute frame index (0-based, warmup frames included in the numbering)
    FrameTimes times;
    double     frameMs    = 0; // start of this frame to start of the next one
    uint64_t   drawCount  = 0;
    uint64_t   indexCount = 0;
};

enum class Phase { NotStarted, Warmup, Measure, Done };

class Recorder
{
public:
    // Call at the start of every frame on the main thread. The first call starts the warmup clock.
    // Returns false once the measured duration has elapsed (benchmark mode only): leave the loop and
    // call Finish(). Outside benchmark mode it always returns true and nothing is stored.
    bool BeginFrame()
    {
        const int64_t t = Now();
        if (m_Phase == Phase::Done) return false;
        if (m_Phase == Phase::NotStarted)
        {
            m_RunStart = t;
            m_Phase    = Phase::Warmup;
            if (config.enabled) m_Rows.reserve(size_t(1) << 18);
        }
        else
        {
            // close the open frame
            if (m_Phase == Phase::Measure && config.enabled)
            {
                Row r;
                r.frame      = m_Frame;
                r.times      = m_Times;
                r.frameMs    = Ms(t - m_FrameStart);
                r.drawCount  = m_Draws;
                r.indexCount = m_Indices;
                m_Rows.push_back(r);
            }
            else
                ++m_WarmupFrames;
            ++m_Frame;
        }
        if (config.enabled)
        {
            if (m_Phase == Phase::Warmup && Seconds(t - m_RunStart) >= config.warmup)
            {
                m_Phase        = Phase::Measure;
                m_MeasureStart = t;
            }
            else if (m_Phase == Phase::Measure && Seconds(t - m_MeasureStart) >= config.duration)
            {
                m_Phase      = Phase::Done;
                m_MeasureEnd = t;
                return false;
            }
        }
        m_FrameStart = t;
        m_Times      = FrameTimes{};
        m_Draws = m_Indices = 0;
        return true;
    }

    // Accumulators of the frame opened by the last BeginFrame.
    FrameTimes& Times() { return m_Times; }

    void SetFrameStats(uint64_t drawCount, uint64_t indexCount)
    {
        m_Draws   = drawCount;
        m_Indices = indexCount;
    }

    // GPU time usually becomes available a few frames late: attribute it to the frame it belongs to.
    // Results for frames outside the measured window are dropped.
    void SetGpuMs(uint64_t frame, double ms)
    {
        if (frame == m_Frame && m_Phase != Phase::Done) { m_Times.gpu = ms; return; }
        if (m_Rows.empty() || frame < m_Rows.front().frame) return;
        const uint64_t i = frame - m_Rows.front().frame;
        if (i < m_Rows.size()) m_Rows[size_t(i)].times.gpu = ms;
    }

    uint64_t FrameIndex() const { return m_Frame; } // absolute index of the open frame
    Phase    GetPhase() const { return m_Phase; }
    bool     Measuring() const { return m_Phase == Phase::Measure; }
    bool     Done() const { return m_Phase == Phase::Done; }

    // True while the frame requested with -capture_frame is open.
    bool CaptureThisFrame() const { return config.captureFrame >= 0 && uint64_t(config.captureFrame) == m_Frame; }
    // Dynamic scene hash right after the simulation update of the capture frame (see DynamicSceneHash).
    void SetCaptureSceneHash(uint64_t h) { m_CaptureHash = h; m_HasCaptureHash = true; }

    const std::vector<Row>& Rows() const { return m_Rows; }
    uint64_t WarmupFrames() const { return m_WarmupFrames; }
    double   MeasuredSeconds() const { return m_Phase == Phase::Done ? Seconds(m_MeasureEnd - m_MeasureStart) : 0.0; }
    bool     HasCaptureHash() const { return m_HasCaptureHash; }
    uint64_t CaptureHash() const { return m_CaptureHash; }

private:
    Phase            m_Phase = Phase::NotStarted;
    int64_t          m_RunStart = 0, m_MeasureStart = 0, m_MeasureEnd = 0, m_FrameStart = 0;
    uint64_t         m_Frame = 0, m_WarmupFrames = 0, m_Draws = 0, m_Indices = 0;
    FrameTimes       m_Times;
    std::vector<Row> m_Rows;
    uint64_t         m_CaptureHash = 0;
    bool             m_HasCaptureHash = false;
};

inline Recorder recorder;

// Frame delta the simulation must use: fixed in benchmark mode, the real one otherwise.
inline float FrameDelta(float realDeltaSeconds) { return config.enabled ? kTimestepF : realDeltaSeconds; }

// ------------------------------------------------------------------ small utilities

inline std::string JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
                else out += char(c);
        }
    }
    return out;
}

inline std::string Hex64(uint64_t v)
{
    char b[24];
    snprintf(b, sizeof(b), "0x%016llx", static_cast<unsigned long long>(v));
    return b;
}

inline std::string Narrow(const wchar_t* w)
{
    char b[512]{};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, b, int(sizeof(b)) - 1, nullptr, nullptr);
    return b;
}

inline bool IsOneOf(const std::string& v, const char* const* list, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (v == list[i]) return true;
    return false;
}

namespace detail
{
inline FILE* OpenForWrite(const std::filesystem::path& p)
{
    FILE* f = nullptr;
    return _wfopen_s(&f, p.c_str(), L"wb") == 0 ? f : nullptr;
}
} // namespace detail

// ------------------------------------------------------------------ failure path

enum class FailKind { Error, Unsupported };

// Runtime facts reported by the renderer.
struct State
{
    bool        adapterSet = false;
    uint64_t    adapterLuid = 0;
    std::string adapterName;
};
inline State state;

// Writes DIR/run.json with "status":"failed". No-op without -output. Does not exit.
inline void WriteFailure(const std::string& reason, FailKind kind = FailKind::Error)
{
    if (config.output.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(config.output), ec);
    const std::filesystem::path path = std::filesystem::path(config.output) / "run.json";
    FILE* f = detail::OpenForWrite(path);
    if (!f) return;
    fprintf(f,
            "{\n  \"status\": \"failed\",\n  \"failure_kind\": \"%s\",\n  \"reason\": \"%s\",\n"
            "  \"renderer\": \"%s\",\n  \"api\": \"%s\",\n  \"binding\": \"%s\",\n  \"requested_binding\": \"%s\",\n"
            "  \"threads\": %d,\n  \"requested_adapter_luid\": \"%llu\",\n  \"command_line\": \"%s\"\n}\n",
            kind == FailKind::Unsupported ? "unsupported" : "error", JsonEscape(reason).c_str(),
            JsonEscape(config.renderer).c_str(), JsonEscape(config.api).c_str(),
            JsonEscape(EffectiveBinding(config)).c_str(), JsonEscape(config.binding).c_str(), config.threads,
            static_cast<unsigned long long>(config.adapterLuid), JsonEscape(GetCommandLineA()).c_str());
    fclose(f);
}

// Writes the failure record, prints the reason and terminates the process immediately with
// kExitFailed / kExitUnsupported (no destructors run: safe to call with live GPU objects and worker threads).
[[noreturn]] inline void Fail(const std::string& reason, FailKind kind = FailKind::Error)
{
    WriteFailure(reason, kind);
    fprintf(stderr, "benchmark %s: %s\n", kind == FailKind::Unsupported ? "unsupported" : "error", reason.c_str());
    fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), UINT(kind == FailKind::Unsupported ? kExitUnsupported : kExitFailed));
    abort();
}

// ------------------------------------------------------------------ command line

enum class ArgResult { NotMine, Consumed, Error };

namespace detail
{
inline bool ParseInt(const char* s, long long lo, long long hi, long long& out)
{
    char* end = nullptr;
    errno     = 0;
    const long long v = strtoll(s, &end, 10);
    if (end == s || *end != 0 || errno != 0 || v < lo || v > hi) return false;
    out = v;
    return true;
}
inline bool ParseU64(const char* s, uint64_t& out)
{
    if (*s == '-' || *s == 0) return false;
    char* end = nullptr;
    errno     = 0;
    const unsigned long long v = strtoull(s, &end, 0); // decimal, or 0x-prefixed hex
    if (end == s || *end != 0 || errno != 0) return false;
    out = v;
    return true;
}
inline bool ParseDouble(const char* s, double lo, double& out)
{
    char* end = nullptr;
    const double v = strtod(s, &end);
    if (end == s || *end != 0 || !std::isfinite(v) || v < lo) return false;
    out = v;
    return true;
}
} // namespace detail

// Tries to consume the contract flag at argv[i] (and its values; `i` is advanced to the last consumed
// token). Returns NotMine for flags this header does not define, Error (with `error` set) for a contract
// flag with a missing or malformed value.
inline ArgResult ParseArg(int argc, const char* const* argv, int& i, Config& c, std::string& error)
{
    const char* a = argv[i];
    auto is = [&](const char* name) { return _stricmp(a, name) == 0; };
    auto need = [&](int n) {
        if (i + n < argc) return true;
        error = std::string("missing value for ") + a;
        return false;
    };
    auto bad = [&](const char* v) {
        error = std::string("invalid value '") + v + "' for " + a;
        return ArgResult::Error;
    };
    long long n = 0;

    if (is("-benchmark")) { c.enabled = true; return ArgResult::Consumed; }
    if (is("-gpu_timing")) { c.gpuTiming = true; return ArgResult::Consumed; }
    if (is("-renderer"))
    {
        if (!need(1)) return ArgResult::Error;
        c.renderer = argv[++i]; c.hasRenderer = true;
        if (!IsOneOf(c.renderer, kRenderers, std::size(kRenderers))) return bad(argv[i]);
        return ArgResult::Consumed;
    }
    if (is("-api"))
    {
        if (!need(1)) return ArgResult::Error;
        c.api = argv[++i]; c.hasApi = true;
        if (!IsOneOf(c.api, kApis, std::size(kApis))) return bad(argv[i]);
        return ArgResult::Consumed;
    }
    if (is("-binding"))
    {
        if (!need(1)) return ArgResult::Error;
        c.binding = argv[++i]; c.hasBinding = true;
        if (!IsOneOf(c.binding, kBindings, std::size(kBindings))) return bad(argv[i]);
        return ArgResult::Consumed;
    }
    if (is("-threads"))
    {
        if (!need(1)) return ArgResult::Error;
        if (!detail::ParseInt(argv[++i], 1, 256, n)) return bad(argv[i]);
        c.threads = int(n); c.hasThreads = true;
        return ArgResult::Consumed;
    }
    if (is("-warmup"))
    {
        if (!need(1)) return ArgResult::Error;
        if (!detail::ParseDouble(argv[++i], 0.0, c.warmup)) return bad(argv[i]);
        return ArgResult::Consumed;
    }
    if (is("-duration"))
    {
        if (!need(1)) return ArgResult::Error;
        if (!detail::ParseDouble(argv[++i], 0.0, c.duration) || c.duration <= 0.0) return bad(argv[i]);
        return ArgResult::Consumed;
    }
    if (is("-window"))
    {
        if (!need(2)) return ArgResult::Error;
        if (!detail::ParseInt(argv[++i], 16, 16384, n)) return bad(argv[i]);
        c.width = int(n);
        if (!detail::ParseInt(argv[++i], 16, 16384, n)) return bad(argv[i]);
        c.height = int(n); c.hasWindow = true;
        return ArgResult::Consumed;
    }
    if (is("-adapter_luid"))
    {
        if (!need(1)) return ArgResult::Error;
        if (!detail::ParseU64(argv[++i], c.adapterLuid)) return bad(argv[i]);
        return ArgResult::Consumed;
    }
    if (is("-output"))
    {
        if (!need(1)) return ArgResult::Error;
        c.output = argv[++i];
        return ArgResult::Consumed;
    }
    if (is("-capture_frame"))
    {
        if (!need(1)) return ArgResult::Error;
        if (!detail::ParseInt(argv[++i], 0, (std::numeric_limits<long long>::max)(), n)) return bad(argv[i]);
        c.captureFrame = n;
        return ArgResult::Consumed;
    }
    return ArgResult::NotMine;
}

// Parses a whole command line. Flags this header does not define are appended to `unknown`
// (or are an error when `unknown` is null).
inline bool ParseCommandLine(int argc, const char* const* argv, Config& c, std::vector<std::string>* unknown, std::string& error)
{
    for (int i = 1; i < argc; ++i)
    {
        switch (ParseArg(argc, argv, i, c, error))
        {
            case ArgResult::Consumed: break;
            case ArgResult::Error: return false;
            case ArgResult::NotMine:
                if (!unknown) { error = std::string("unrecognized argument '") + argv[i] + "'"; return false; }
                unknown->push_back(argv[i]);
                break;
        }
    }
    return true;
}

inline const char* Usage()
{
    return "  -benchmark                 benchmark mode (no GUI sprites, fixed timestep, auto-exit)\n"
           "  -renderer native|nvrhi\n"
           "  -api d3d11|d3d12|vk\n"
           "  -binding dyn|mut|tex_mut|tex_mut_pc|bindless\n"
           "  -threads N                 1 = single-threaded rendering\n"
           "  -warmup SECONDS  -duration SECONDS\n"
           "  -window W H                default 1080 720\n"
           "  -adapter_luid N            fail if the created device is on another adapter\n"
           "  -gpu_timing                optional GPU timestamp queries\n"
           "  -output DIR                writes DIR/frames.csv and DIR/run.json\n"
           "  -capture_frame N           parity check: write DIR/capture.bmp for absolute frame N\n";
}

// docs/PLAN.md "Binding modes". Returns false with `reason` when the combination is not expressible.
// Renderers may reject further combinations themselves with Fail(..., FailKind::Unsupported).
inline bool CombinationSupported(const std::string& renderer, const std::string& api, const std::string& binding, std::string& reason)
{
    if (renderer == "native")
    {
        if (api == "vk") { reason = "there is no native Vulkan renderer"; return false; }
        return true; // -binding is ignored and recorded as "original"
    }
    if (renderer == "nvrhi")
    {
        if (binding == "dyn") { reason = "dyn is not expressible in nvrhi: binding sets are immutable"; return false; }
        if (binding == "bindless" && api == "d3d11") { reason = "bindless requires d3d12 or vk"; return false; }
        return true;
    }
    reason = "unknown renderer '" + renderer + "'";
    return false;
}

enum class Check { Ok, Invalid, Unsupported };

// Validates a parsed configuration for an executable that implements `supportedRenderers`.
// Outside benchmark mode nothing is required.
inline Check Validate(const Config& c, const char* const* supportedRenderers, size_t count, std::string& error)
{
    if (!c.enabled) return Check::Ok;
    if (c.renderer.empty()) { error = "-renderer is required in benchmark mode"; return Check::Invalid; }
    if (c.api.empty()) { error = "-api is required in benchmark mode"; return Check::Invalid; }
    if (c.output.empty()) { error = "-output is required in benchmark mode"; return Check::Invalid; }
    if (!IsOneOf(c.renderer, supportedRenderers, count))
    {
        error = "renderer '" + c.renderer + "' is not implemented by this executable";
        return Check::Unsupported;
    }
    if (c.renderer != "native" && c.binding.empty()) { error = "-binding is required for renderer " + c.renderer; return Check::Invalid; }
    if (!CombinationSupported(c.renderer, c.api, c.binding, error)) return Check::Unsupported;
    return Check::Ok;
}

inline void ValidateOrFail(const Config& c, const char* const* supportedRenderers, size_t count)
{
    std::string error;
    switch (Validate(c, supportedRenderers, count, error))
    {
        case Check::Ok: return;
        case Check::Invalid: Fail(error, FailKind::Error);
        case Check::Unsupported: Fail(error, FailKind::Unsupported);
    }
}

// ------------------------------------------------------------------ adapters (DXGI LUID)

// Decimal form used by -adapter_luid and run.json.
inline uint64_t LuidToU64(const LUID& l) { return (uint64_t(uint32_t(l.HighPart)) << 32) | uint64_t(l.LowPart); }
inline LUID     U64ToLuid(uint64_t v)
{
    LUID l;
    l.LowPart  = DWORD(v & 0xffffffffull);
    l.HighPart = LONG(uint32_t(v >> 32));
    return l;
}
// For VkPhysicalDeviceIDProperties::deviceLUID (check deviceLUIDValid) and donut::app::AdapterInfo::luid.
inline uint64_t LuidFromBytes(const uint8_t bytes[8])
{
    LUID l;
    memcpy(&l, bytes, sizeof(l));
    return LuidToU64(l);
}

struct AdapterDesc
{
    unsigned    index = 0; // IDXGIFactory1::EnumAdapters1 order
    uint64_t    luid  = 0;
    std::string name;
    unsigned    vendorId = 0, deviceId = 0;
    uint64_t    dedicatedVideoMemory = 0;
    bool        software = false;
};

inline std::vector<AdapterDesc> EnumerateAdapters()
{
    std::vector<AdapterDesc> out;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return out;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(adapter->GetDesc1(&d)))
        {
            AdapterDesc a;
            a.index = i;
            a.luid  = LuidToU64(d.AdapterLuid);
            a.name  = Narrow(d.Description);
            a.vendorId = d.VendorId;
            a.deviceId = d.DeviceId;
            a.dedicatedVideoMemory = d.DedicatedVideoMemory;
            a.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            out.push_back(a);
        }
        adapter->Release();
    }
    factory->Release();
    return out;
}

// Returns the adapter with this LUID (caller releases it) or nullptr.
inline IDXGIAdapter1* FindDxgiAdapter(uint64_t luid)
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return nullptr;
    IDXGIAdapter1* adapter = nullptr;
    IDXGIAdapter1* found   = nullptr;
    for (UINT i = 0; !found && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(adapter->GetDesc1(&d)) && LuidToU64(d.AdapterLuid) == luid) found = adapter;
        else adapter->Release();
    }
    factory->Release();
    return found;
}

// Index in EnumAdapters1 order, or -1.
inline int FindDxgiAdapterIndex(uint64_t luid)
{
    for (const auto& a : EnumerateAdapters())
        if (a.luid == luid) return int(a.index);
    return -1;
}

// Records the adapter the device was actually created on and checks it against -adapter_luid.
// Mandatory when -adapter_luid was given: Finish() fails if the renderer never reported it.
inline bool SetActualAdapter(uint64_t luid, std::string& error)
{
    state.adapterSet  = true;
    state.adapterLuid = luid;
    state.adapterName.clear();
    for (const auto& a : EnumerateAdapters())
        if (a.luid == luid) state.adapterName = a.name;
    if (config.adapterLuid != 0 && config.adapterLuid != luid)
    {
        error = "created device is on adapter LUID " + std::to_string(luid) + " (" + state.adapterName +
                "), requested " + std::to_string(config.adapterLuid);
        return false;
    }
    return true;
}
inline bool SetActualAdapter(IDXGIAdapter* adapter, std::string& error)
{
    DXGI_ADAPTER_DESC d{};
    if (!adapter || FAILED(adapter->GetDesc(&d))) { error = "cannot inspect the DXGI adapter"; return false; }
    return SetActualAdapter(LuidToU64(d.AdapterLuid), error);
}
inline void SetActualAdapterOrFail(uint64_t luid)
{
    std::string error;
    if (!SetActualAdapter(luid, error)) Fail(error);
}
inline void SetActualAdapterOrFail(IDXGIAdapter* adapter)
{
    std::string error;
    if (!SetActualAdapter(adapter, error)) Fail(error);
}

// ------------------------------------------------------------------ scene hash (FNV-1a 64)

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime  = 1099511628211ull;

inline uint64_t HashBytes(uint64_t hash, const void* data, size_t size)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * kFnvPrime;
    return hash;
}
template <class T> inline void HashValue(uint64_t& hash, const T& value) { hash = HashBytes(hash, &value, sizeof(value)); }

// `Sim` is AsteroidsSimulation (simulation.h of the reference sample); a template so that this header
// does not depend on it. Hashes the per-asteroid static data (field by field, no padding) and the
// shared mesh vertices/indices. Must be identical for every renderer.
template <class Sim> inline uint64_t StaticSceneHash(Sim& sim, unsigned asteroidCount)
{
    uint64_t hash = kFnvOffset;
    HashValue(hash, asteroidCount);
    const auto* s = sim.StaticData();
    for (unsigned i = 0; i < asteroidCount; ++i)
    {
        const auto& a = s[i];
        HashValue(hash, a.surfaceColor); HashValue(hash, a.deepColor); HashValue(hash, a.spinAxis);
        HashValue(hash, a.scale); HashValue(hash, a.spinVelocity); HashValue(hash, a.orbitVelocity);
        HashValue(hash, a.vertexStart); HashValue(hash, a.textureIndex);
    }
    const auto* mesh = sim.Meshes();
    const uint64_t vertexCount = mesh->vertices.size(), indexCount = mesh->indices.size();
    HashValue(hash, vertexCount); HashValue(hash, indexCount);
    if (vertexCount) hash = HashBytes(hash, mesh->vertices.data(), size_t(vertexCount) * sizeof(mesh->vertices[0]));
    if (indexCount) hash = HashBytes(hash, mesh->indices.data(), size_t(indexCount) * sizeof(mesh->indices[0]));
    return hash;
}

// World matrices and LOD selection after the last update. Comparable between renderers only at the
// same absolute frame (see -capture_frame); the value at the end of a timed run depends on the frame count.
template <class Sim> inline uint64_t DynamicSceneHash(Sim& sim, unsigned asteroidCount)
{
    uint64_t hash = kFnvOffset;
    const auto* d = sim.DynamicData();
    for (unsigned i = 0; i < asteroidCount; ++i)
    {
        const auto& a = d[i];
        HashValue(hash, a.world); HashValue(hash, a.indexStart); HashValue(hash, a.indexCount);
    }
    return hash;
}

// ------------------------------------------------------------------ results

struct RunInfo
{
    int      width = 0, height = 0;   // actual back-buffer size
    int      threads = 0;             // threads actually used for command recording (must equal -threads)
    uint64_t asteroids = 0, meshes = 0, textures = 0;
    uint64_t staticSceneHash = 0;
    uint64_t finalDynamicSceneHash = 0;
    std::vector<std::pair<std::string, std::string>> extra; // renderer-specific string facts, written as "extra": {...}
};

inline double Median(std::vector<double> v)
{
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    const size_t n = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + n, v.end());
    double m = v[n];
    if (v.size() % 2 == 0) m = 0.5 * (m + *std::max_element(v.begin(), v.begin() + n));
    return m;
}

// Writes DIR/frames.csv, then DIR/run.json ("status":"ok"; written to a temporary file and renamed, so an
// existing ok run.json implies a complete frames.csv).
inline bool WriteResults(const Config& c, const Recorder& rec, const RunInfo& info, std::string& error)
{
    if (c.output.empty()) { error = "no -output directory"; return false; }
    const std::filesystem::path dir = std::filesystem::path(c.output);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    const auto& rows = rec.Rows();
    {
        FILE* f = detail::OpenForWrite(dir / "frames.csv");
        if (!f) { error = "cannot open frames.csv in " + c.output; return false; }
        fprintf(f, "%s\n", kCsvHeader);
        for (const Row& r : rows)
        {
            const FrameTimes& t = r.times;
            fprintf(f, "%llu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,", static_cast<unsigned long long>(r.frame), t.update, t.render,
                    t.submit, t.wait, t.present, r.frameMs);
            if (std::isfinite(t.gpu)) fprintf(f, "%.6f", t.gpu);
            fprintf(f, ",%llu,%llu,%.6f\n", static_cast<unsigned long long>(r.drawCount),
                    static_cast<unsigned long long>(r.indexCount), t.gc);
        }
        const bool bad = ferror(f) != 0;
        if (fclose(f) != 0 || bad) { error = "cannot write frames.csv"; return false; }
    }

    std::vector<double> cpu, frame, gpu;
    cpu.reserve(rows.size()); frame.reserve(rows.size());
    for (const Row& r : rows)
    {
        cpu.push_back(r.times.render + r.times.submit);
        frame.push_back(r.frameMs);
        if (std::isfinite(r.times.gpu)) gpu.push_back(r.times.gpu);
    }
    auto num = [](double v) {
        char b[40];
        if (std::isfinite(v)) snprintf(b, sizeof(b), "%.9g", v); else snprintf(b, sizeof(b), "null");
        return std::string(b);
    };

    SYSTEMTIME st;
    GetSystemTime(&st);
    char when[40];
    snprintf(when, sizeof(when), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);

    std::string j = "{\n";
    auto str = [&](const char* k, const std::string& v) { j += "  \"" + std::string(k) + "\": \"" + JsonEscape(v) + "\",\n"; };
    auto raw = [&](const char* k, const std::string& v) { j += "  \"" + std::string(k) + "\": " + v + ",\n"; };
    str("status", "ok");
    str("renderer", c.renderer);
    str("api", c.api);
    str("binding", EffectiveBinding(c));
    str("requested_binding", c.binding);
    raw("threads", std::to_string(info.threads));
    raw("requested_threads", std::to_string(c.threads));
    raw("width", std::to_string(info.width));
    raw("height", std::to_string(info.height));
    raw("requested_width", std::to_string(c.width));
    raw("requested_height", std::to_string(c.height));
    raw("asteroids", std::to_string(info.asteroids));
    raw("meshes", std::to_string(info.meshes));
    raw("textures", std::to_string(info.textures));
    raw("seed", std::to_string(kSeed));
    raw("timestep", num(kTimestep));
    raw("warmup_seconds", num(c.warmup));
    raw("duration_seconds", num(c.duration));
    raw("measured_seconds", num(rec.MeasuredSeconds()));
    raw("warmup_frame_count", std::to_string(rec.WarmupFrames()));
    raw("frame_count", std::to_string(rows.size()));
    raw("vsync", "false");
    raw("validation", "false");
    raw("gpu_timing", c.gpuTiming ? "true" : "false");
    raw("gpu_frame_count", std::to_string(gpu.size()));
    str("adapter_luid", std::to_string(state.adapterLuid));
    str("requested_adapter_luid", std::to_string(c.adapterLuid));
    str("adapter", state.adapterName);
    str("static_scene_hash", Hex64(info.staticSceneHash));
    str("final_dynamic_scene_hash", Hex64(info.finalDynamicSceneHash));
    raw("capture_frame", std::to_string(c.captureFrame));
    if (rec.HasCaptureHash()) str("capture_dynamic_scene_hash", Hex64(rec.CaptureHash()));
    raw("median_cpu_render_ms", num(Median(cpu)));
    raw("median_frame_ms", num(Median(frame)));
    raw("median_gpu_ms", num(Median(gpu)));
    raw("qpc_frequency", std::to_string(Frequency()));
#ifdef NDEBUG
    str("build", "release");
#else
    str("build", "debug");
#endif
    raw("msc_ver", std::to_string(_MSC_VER));
    str("executable", Narrow(exe));
    str("command_line", GetCommandLineA());
    str("finished_utc", when);
    j += "  \"extra\": {";
    for (size_t i = 0; i < info.extra.size(); ++i)
        j += std::string(i ? ", " : "") + "\"" + JsonEscape(info.extra[i].first) + "\": \"" + JsonEscape(info.extra[i].second) + "\"";
    j += "}\n}\n";

    const std::filesystem::path tmp = dir / "run.json.tmp", dst = dir / "run.json";
    FILE* f = detail::OpenForWrite(tmp);
    if (!f) { error = "cannot open run.json in " + c.output; return false; }
    const bool ok = fwrite(j.data(), 1, j.size(), f) == j.size();
    if (fclose(f) != 0 || !ok) { error = "cannot write run.json"; return false; }
    if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) { error = "cannot rename run.json.tmp"; return false; }
    return true;
}

// Benchmark mode: keep the window above all others without activating it. A window started from a background
// process is otherwise opened BEHIND the active window whenever Windows denies it the foreground (it does for some
// time after any user input); a covered window is not composed, its presents are dropped and nothing is paced, so
// the run would not be comparable with one whose window was displayed (docs/LIMITATIONS.md, frame pacing).
inline void KeepWindowOnTop(HWND window)
{
    if (window) SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Desktop facts that decide whether the compositor paces presentation (docs/LIMITATIONS.md, frame pacing).
// Call at the end of the measured period, while the window still exists:
//   display_refresh_hz       refresh rate of the monitor the window is on (scripts/analyze.py flags a run as paced
//                            when its frame time sits on a multiple of the refresh period)
//   window_foreground        the window was the foreground window
//   window_visible_fraction  share of a 5 x 5 grid of points inside the window at which the window is the topmost
//                            one (0 when another window, or the lock screen, covers it: presents are then dropped
//                            by the compositor and nothing is paced)
inline void AddDesktopFacts(RunInfo& info, HWND window)
{
    auto add = [&](const char* key, const std::string& value) { info.extra.emplace_back(key, value); };
    if (!window || !IsWindow(window))
    {
        add("display_refresh_hz", "unknown");
        return;
    }
    DWORD hz = 0;
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor) &&
        EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode))
        hz = mode.dmDisplayFrequency;
    add("display_refresh_hz", hz > 1 ? std::to_string(hz) : "unknown");
    add("window_foreground", GetForegroundWindow() == window ? "true" : "false");

    RECT rect{};
    int hit = 0, total = 0;
    if (GetWindowRect(window, &rect))
        for (int i = 1; i <= 5; ++i)
            for (int k = 1; k <= 5; ++k)
            {
                const POINT p{rect.left + (rect.right - rect.left) * i / 6, rect.top + (rect.bottom - rect.top) * k / 6};
                const HWND at = WindowFromPoint(p);
                ++total;
                if (at && (at == window || GetAncestor(at, GA_ROOT) == window)) ++hit;
            }
    char text[32];
    snprintf(text, sizeof(text), "%.2f", total ? double(hit) / total : 0.0);
    add("window_visible_fraction", text);
    add("window_minimized", IsIconic(window) ? "true" : "false");
    add("window_topmost", (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) ? "true" : "false");
}

// End of a benchmark run: checks the contract invariants, writes both files and returns the process exit
// code (kExitOk only after both files were written). On any violation it writes the failure record and
// returns kExitFailed. Call after the frame loop ended because BeginFrame() returned false.
inline int Finish(const RunInfo& info)
{
    std::string error;
    if (!config.enabled) error = "Finish() called outside benchmark mode";
    else if (!recorder.Done()) error = "run ended before the measured duration elapsed";
    else if (recorder.Rows().empty()) error = "no frames were measured";
    else if (config.adapterLuid != 0 && !state.adapterSet) error = "-adapter_luid given but the renderer did not report its adapter";
    else if (info.threads != config.threads)
        error = "renderer used " + std::to_string(info.threads) + " threads, requested " + std::to_string(config.threads);
    else if (WriteResults(config, recorder, info, error))
    {
        std::vector<double> cpu, frame;
        for (const Row& r : recorder.Rows()) { cpu.push_back(r.times.render + r.times.submit); frame.push_back(r.frameMs); }
        printf("benchmark ok: %s %s %s threads=%d frames=%zu median render+submit=%.3f ms frame=%.3f ms -> %s\n",
               config.renderer.c_str(), config.api.c_str(), EffectiveBinding(config).c_str(), info.threads,
               recorder.Rows().size(), Median(cpu), Median(frame), config.output.c_str());
        fflush(stdout);
        return kExitOk;
    }
    WriteFailure(error);
    fprintf(stderr, "benchmark error: %s\n", error.c_str());
    fflush(stderr);
    return kExitFailed;
}

// ------------------------------------------------------------------ frame capture (parity checks)

// Writes DIR/capture.bmp (24-bit) from a top-down 32-bit image. `bgra` = true for B8G8R8A8 sources,
// false for R8G8B8A8. Alpha is dropped.
inline bool WriteCapture(const void* pixels, int width, int height, size_t rowPitch, bool bgra, std::string& error)
{
    if (config.output.empty()) { error = "no -output directory"; return false; }
    const std::filesystem::path dir = std::filesystem::path(config.output);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    FILE* f = detail::OpenForWrite(dir / "capture.bmp");
    if (!f) { error = "cannot open capture.bmp"; return false; }
    const uint32_t stride = (uint32_t(width) * 3u + 3u) & ~3u;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType    = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize    = fh.bfOffBits + stride * uint32_t(height);
    ih.biSize    = sizeof(ih);
    ih.biWidth   = width;
    ih.biHeight  = -height; // top-down
    ih.biPlanes  = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> line(stride, 0);
    for (int y = 0; y < height; ++y)
    {
        const uint8_t* src = static_cast<const uint8_t*>(pixels) + size_t(y) * rowPitch;
        for (int x = 0; x < width; ++x)
        {
            line[size_t(x) * 3 + 0] = src[x * 4 + (bgra ? 0 : 2)];
            line[size_t(x) * 3 + 1] = src[x * 4 + 1];
            line[size_t(x) * 3 + 2] = src[x * 4 + (bgra ? 2 : 0)];
        }
        fwrite(line.data(), 1, stride, f);
    }
    const bool bad = ferror(f) != 0;
    if (fclose(f) != 0 || bad) { error = "cannot write capture.bmp"; return false; }
    return true;
}

} // namespace Benchmark

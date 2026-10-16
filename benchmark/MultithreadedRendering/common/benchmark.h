#pragma once
// Benchmark contract shared by mt_rendering_native (D3D12) and mt_rendering_nvrhi (Donut + nvrhi, D3D12 / Vulkan).
// Same output files and columns as the Asteroids benchmark (../../Asteroids/common/benchmark.h), so the two
// benchmarks can be read with the same tools; the dimensions differ: -mode replaces -binding.
//
//   -benchmark -renderer native|nvrhi -api d3d12|vk -mode st|mt_scene|mt_chunk -threads N
//   -warmup S -duration S -window W H -adapter_luid N -output DIR
//
// Threads: the number of command-recording threads including the main thread.
//   st        single-threaded: one command list records every pass (threads must be 1)
//   mt_scene  one job per shadow map and per mirror (5 jobs) on the worker threads, the direct
//             view on the main thread; one list per job (threads >= 2)
//   mt_chunk  every pass is split into `threads` chunks of the draw list, one list per chunk,
//             recorded concurrently and executed in chunk order (threads >= 2)
//
// Host integration: see the Asteroids header. All state is `inline`; one TU owns main().

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
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Benchmark
{

constexpr double kTimestep      = 1.0 / 60.0; // fixed simulation step in benchmark mode
constexpr float  kTimestepF     = 1.0f / 60.0f;
constexpr int    kDefaultWidth  = 1280;
constexpr int    kDefaultHeight = 720;

constexpr int kExitOk          = 0;
constexpr int kExitFailed      = 2;
constexpr int kExitUnsupported = 3;

constexpr const char* kRenderers[] = {"native", "nvrhi"};
constexpr const char* kApis[]      = {"d3d12", "vk"};
constexpr const char* kModes[]     = {"st", "mt_scene", "mt_chunk"};

constexpr const char* kCsvHeader =
    "frame,update_ms,render_ms,submit_ms,wait_ms,present_ms,frame_ms,gpu_ms,draw_count,index_count,gc_ms";

struct Config
{
    bool        enabled  = false;
    std::string renderer;
    std::string api;
    std::string mode;
    int         threads  = 1;
    double      warmup   = 5.0;
    double      duration = 20.0;
    int         width    = kDefaultWidth;
    int         height   = kDefaultHeight;
    uint64_t    adapterLuid = 0;
    std::string output;

    bool hasRenderer = false, hasApi = false, hasMode = false, hasThreads = false, hasWindow = false;
};

inline Config config;

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

// Attributes consecutive main-thread segments to columns with one QPC read per boundary.
class Lap
{
    int64_t m_Last;

public:
    Lap() : m_Last(Now()) {}
    void To(double& column)
    {
        const int64_t now = Now();
        column += Ms(now - m_Last);
        m_Last = now;
    }
    void Skip() { m_Last = Now(); }
};

// ------------------------------------------------------------------ per-frame data

struct FrameTimes
{
    double update  = 0; // scene animation (lights, camera)
    double render  = 0; // command recording until every list is recorded (workers joined)
    double submit  = 0; // ExecuteCommandLists / executeCommandLists
    double wait    = 0; // frame fence waits
    double present = 0; // Present
    double gpu     = std::numeric_limits<double>::quiet_NaN(); // not measured by this benchmark
    double gc      = 0; // nvrhi: IDevice::runGarbageCollection (part of wait)
};

struct Row
{
    uint64_t   frame = 0;
    FrameTimes times;
    double     frameMs    = 0;
    uint64_t   drawCount  = 0;
    uint64_t   indexCount = 0;
};

enum class Phase { NotStarted, Warmup, Measure, Done };

class Recorder
{
public:
    bool BeginFrame()
    {
        const int64_t t = Now();
        if (m_Phase == Phase::Done) return false;
        if (m_Phase == Phase::NotStarted)
        {
            m_RunStart = t;
            m_Phase    = Phase::Warmup;
            if (config.enabled) m_Rows.reserve(size_t(1) << 16);
        }
        else
        {
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

    FrameTimes& Times() { return m_Times; }
    void SetFrameStats(uint64_t drawCount, uint64_t indexCount) { m_Draws = drawCount; m_Indices = indexCount; }

    uint64_t FrameIndex() const { return m_Frame; }
    Phase    GetPhase() const { return m_Phase; }
    bool     Measuring() const { return m_Phase == Phase::Measure; }
    bool     Done() const { return m_Phase == Phase::Done; }

    const std::vector<Row>& Rows() const { return m_Rows; }
    uint64_t WarmupFrames() const { return m_WarmupFrames; }
    double   MeasuredSeconds() const { return m_Phase == Phase::Done ? Seconds(m_MeasureEnd - m_MeasureStart) : 0.0; }

private:
    Phase            m_Phase = Phase::NotStarted;
    int64_t          m_RunStart = 0, m_MeasureStart = 0, m_MeasureEnd = 0, m_FrameStart = 0;
    uint64_t         m_Frame = 0, m_WarmupFrames = 0, m_Draws = 0, m_Indices = 0;
    FrameTimes       m_Times;
    std::vector<Row> m_Rows;
};

inline Recorder recorder;

inline float FrameDelta(float realDeltaSeconds) { return config.enabled ? kTimestepF : realDeltaSeconds; }

// ------------------------------------------------------------------ utilities

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

struct State
{
    bool        adapterSet = false;
    uint64_t    adapterLuid = 0;
    std::string adapterName;
};
inline State state;

inline void WriteFailure(const std::string& reason, FailKind kind = FailKind::Error)
{
    if (config.output.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(config.output), ec);
    FILE* f = detail::OpenForWrite(std::filesystem::path(config.output) / "run.json");
    if (!f) return;
    fprintf(f,
            "{\n  \"status\": \"failed\",\n  \"failure_kind\": \"%s\",\n  \"reason\": \"%s\",\n"
            "  \"renderer\": \"%s\",\n  \"api\": \"%s\",\n  \"mode\": \"%s\",\n"
            "  \"threads\": %d,\n  \"requested_adapter_luid\": \"%llu\",\n  \"command_line\": \"%s\"\n}\n",
            kind == FailKind::Unsupported ? "unsupported" : "error", JsonEscape(reason).c_str(),
            JsonEscape(config.renderer).c_str(), JsonEscape(config.api).c_str(), JsonEscape(config.mode).c_str(),
            config.threads, static_cast<unsigned long long>(config.adapterLuid), JsonEscape(GetCommandLineA()).c_str());
    fclose(f);
}

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
    const unsigned long long v = strtoull(s, &end, 0);
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
    if (is("-mode"))
    {
        if (!need(1)) return ArgResult::Error;
        c.mode = argv[++i]; c.hasMode = true;
        if (!IsOneOf(c.mode, kModes, std::size(kModes))) return bad(argv[i]);
        return ArgResult::Consumed;
    }
    if (is("-threads"))
    {
        if (!need(1)) return ArgResult::Error;
        if (!detail::ParseInt(argv[++i], 1, 64, n)) return bad(argv[i]);
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
    return ArgResult::NotMine;
}

inline const char* Usage()
{
    return "  -benchmark                 benchmark mode (fixed timestep, scripted camera, auto-exit)\n"
           "  -renderer native|nvrhi\n"
           "  -api d3d12|vk\n"
           "  -mode st|mt_scene|mt_chunk\n"
           "  -threads N                 recording threads including the main thread (st: 1)\n"
           "  -warmup SECONDS  -duration SECONDS\n"
           "  -window W H                default 1280 720\n"
           "  -adapter_luid N            fail if the created device is on another adapter\n"
           "  -output DIR                writes DIR/frames.csv and DIR/run.json\n";
}

inline bool CombinationSupported(const std::string& renderer, const std::string& api, const std::string& mode, int threads, std::string& reason)
{
    if (renderer == "native" && api == "vk") { reason = "there is no native Vulkan renderer"; return false; }
    if (mode == "st" && threads != 1) { reason = "-mode st records on the main thread only (-threads 1)"; return false; }
    if (mode != "st" && threads < 2) { reason = "-mode " + mode + " needs at least 2 threads"; return false; }
    return true;
}

enum class Check { Ok, Invalid, Unsupported };

inline Check Validate(const Config& c, const char* const* supportedRenderers, size_t count, std::string& error)
{
    if (!c.enabled) return Check::Ok;
    if (c.renderer.empty()) { error = "-renderer is required in benchmark mode"; return Check::Invalid; }
    if (c.api.empty()) { error = "-api is required in benchmark mode"; return Check::Invalid; }
    if (c.mode.empty()) { error = "-mode is required in benchmark mode"; return Check::Invalid; }
    if (c.output.empty()) { error = "-output is required in benchmark mode"; return Check::Invalid; }
    if (!IsOneOf(c.renderer, supportedRenderers, count))
    {
        error = "renderer '" + c.renderer + "' is not implemented by this executable";
        return Check::Unsupported;
    }
    if (!CombinationSupported(c.renderer, c.api, c.mode, c.threads, error)) return Check::Unsupported;
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

inline uint64_t LuidToU64(const LUID& l) { return (uint64_t(uint32_t(l.HighPart)) << 32) | uint64_t(l.LowPart); }
inline uint64_t LuidFromBytes(const uint8_t bytes[8])
{
    LUID l;
    memcpy(&l, bytes, sizeof(l));
    return LuidToU64(l);
}

struct AdapterDesc
{
    unsigned    index = 0;
    uint64_t    luid  = 0;
    std::string name;
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
inline void SetActualAdapterOrFail(uint64_t luid)
{
    std::string error;
    if (!SetActualAdapter(luid, error)) Fail(error);
}

// ------------------------------------------------------------------ hashing (FNV-1a 64)

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime  = 1099511628211ull;

inline uint64_t HashBytes(uint64_t hash, const void* data, size_t size)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * kFnvPrime;
    return hash;
}

// ------------------------------------------------------------------ results

struct RunInfo
{
    int      width = 0, height = 0;
    int      threads = 0;           // recording threads actually used (must equal -threads)
    uint64_t meshes = 0, subsets = 0, textures = 0, passes = 0;
    uint64_t sceneHash = 0;         // hash of the loaded geometry and material list
    std::vector<std::pair<std::string, std::string>> extra;
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
    std::vector<double> cpu, frame, render, submit, wait;
    for (const Row& r : rows)
    {
        cpu.push_back(r.times.render + r.times.submit);
        frame.push_back(r.frameMs);
        render.push_back(r.times.render);
        submit.push_back(r.times.submit);
        wait.push_back(r.times.wait);
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
    str("benchmark", "multithreaded_rendering");
    str("renderer", c.renderer);
    str("api", c.api);
    str("mode", c.mode);
    raw("threads", std::to_string(info.threads));
    raw("requested_threads", std::to_string(c.threads));
    raw("width", std::to_string(info.width));
    raw("height", std::to_string(info.height));
    raw("meshes", std::to_string(info.meshes));
    raw("subsets", std::to_string(info.subsets));
    raw("textures", std::to_string(info.textures));
    raw("passes_per_frame", std::to_string(info.passes));
    str("scene_time", "wall clock since the first frame");
    raw("warmup_seconds", num(c.warmup));
    raw("duration_seconds", num(c.duration));
    raw("measured_seconds", num(rec.MeasuredSeconds()));
    raw("warmup_frame_count", std::to_string(rec.WarmupFrames()));
    raw("frame_count", std::to_string(rows.size()));
    raw("vsync", "false");
    str("adapter_luid", std::to_string(state.adapterLuid));
    str("requested_adapter_luid", std::to_string(c.adapterLuid));
    str("adapter", state.adapterName);
    str("scene_hash", Hex64(info.sceneHash));
    raw("median_cpu_render_ms", num(Median(cpu)));
    raw("median_render_ms", num(Median(render)));
    raw("median_submit_ms", num(Median(submit)));
    raw("median_wait_ms", num(Median(wait)));
    raw("median_frame_ms", num(Median(frame)));
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

// Writes a 24-bit BMP from a top-down 32-bit image (`bgra` = true for B8G8R8A8 sources, false for R8G8B8A8).
inline bool WriteBmp(const std::filesystem::path& path, const void* pixels, int width, int height, size_t rowPitch, bool bgra, std::string& error)
{
    FILE* f = detail::OpenForWrite(path);
    if (!f) { error = "cannot open " + path.string(); return false; }
    const uint32_t stride = (uint32_t(width) * 3u + 3u) & ~3u;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + stride * uint32_t(height);
    ih.biSize = sizeof(ih);
    ih.biWidth = width;
    ih.biHeight = -height; // top-down
    ih.biPlanes = 1;
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
    if (fclose(f) != 0 || bad) { error = "cannot write " + path.string(); return false; }
    return true;
}

// Benchmark mode: keep the window displayed even when Windows denies this process the foreground
// (a covered window is not composed and its presents are dropped; see the Asteroids LIMITATIONS.md).
inline void KeepWindowOnTop(HWND window)
{
    if (window) SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

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
}

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
               config.renderer.c_str(), config.api.c_str(), config.mode.c_str(), info.threads,
               recorder.Rows().size(), Median(cpu), Median(frame), config.output.c_str());
        fflush(stdout);
        return kExitOk;
    }
    WriteFailure(error);
    fprintf(stderr, "benchmark error: %s\n", error.c_str());
    fflush(stderr);
    return kExitFailed;
}

} // namespace Benchmark

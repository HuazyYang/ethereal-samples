// Compile and behaviour check of the shared benchmark headers, and the adapter LUID query tool:
//   benchmark_selftest                 runs the checks (exit 0 = pass)
//   benchmark_selftest -list_adapters  prints "<luid decimal>\t<index>\t<vendor:device>\t<name>" per DXGI adapter
#include "benchmark.h"
#include "benchmark_d3d11.h"
#include "benchmark_d3d12.h"

#include <fstream>
#include <sstream>

namespace
{
int gFailures = 0;
#define CHECK(x) do { if (!(x)) { ++gFailures; printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

struct FakeStatic
{
    float    surfaceColor[3], deepColor[3], spinAxis[4], scale, spinVelocity, orbitVelocity;
    unsigned vertexStart, textureIndex;
};
struct FakeDynamic
{
    float    world[16];
    unsigned indexStart, indexCount;
};
struct FakeMesh
{
    std::vector<float>          vertices;
    std::vector<unsigned short> indices;
};
struct FakeSim
{
    std::vector<FakeStatic>  s;
    std::vector<FakeDynamic> d;
    FakeMesh                 m;
    const FakeStatic*  StaticData() const { return s.data(); }
    const FakeDynamic* DynamicData() const { return d.data(); }
    const FakeMesh*    Meshes() { return &m; }
};

std::string ReadFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
} // namespace

int main(int argc, char** argv)
{
    using namespace Benchmark;
    if (argc > 1 && _stricmp(argv[1], "-list_adapters") == 0)
    {
        for (const AdapterDesc& a : EnumerateAdapters())
            printf("%llu\t%u\t%04x:%04x\t%s%s\n", static_cast<unsigned long long>(a.luid), a.index, a.vendorId, a.deviceId,
                   a.name.c_str(), a.software ? " (software)" : "");
        return 0;
    }

    // command line
    {
        const char* args[] = {"exe", "-benchmark", "-renderer", "nvrhi", "-api", "vk", "-binding", "tex_mut_pc", "-threads", "8",
                              "-warmup", "0.05", "-duration", "0.1", "-window", "640", "360", "-adapter_luid", "4294967298",
                              "-gpu_timing", "-output", "out", "-legacy"};
        Config c;
        std::vector<std::string> unknown;
        std::string err;
        CHECK(ParseCommandLine(int(std::size(args)), args, c, &unknown, err));
        CHECK(c.enabled && c.renderer == "nvrhi" && c.api == "vk" && c.binding == "tex_mut_pc" && c.threads == 8);
        CHECK(c.warmup == 0.05 && c.duration == 0.1 && c.width == 640 && c.height == 360 && c.gpuTiming && c.output == "out");
        CHECK(c.adapterLuid == 4294967298ull && unknown.size() == 1 && unknown[0] == "-legacy");
        CHECK(!ParseCommandLine(int(std::size(args)), args, c, nullptr, err));

        const char* badApi[] = {"exe", "-api", "gl"};
        Config c2;
        CHECK(!ParseCommandLine(3, badApi, c2, nullptr, err) && !err.empty());
        const char* missing[] = {"exe", "-window", "640"};
        CHECK(!ParseCommandLine(3, missing, c2, nullptr, err));

        const char* const nvrhiOnly[] = {"nvrhi"};
        const char* const reference[] = {"native"};
        CHECK(Validate(c, nvrhiOnly, 1, err) == Check::Ok);
        CHECK(Validate(c, reference, 1, err) == Check::Unsupported);
        Config c3 = c;
        c3.binding = "dyn";
        CHECK(Validate(c3, nvrhiOnly, 1, err) == Check::Unsupported);
        c3.renderer = "native";
        CHECK(Validate(c3, reference, 1, err) == Check::Unsupported); // native vk
        c3.api = "d3d12";
        CHECK(Validate(c3, reference, 1, err) == Check::Ok && EffectiveBinding(c3) == "original");
        c3.renderer = "nvrhi"; c3.binding = "bindless"; c3.api = "d3d11";
        CHECK(Validate(c3, nvrhiOnly, 1, err) == Check::Unsupported);
        c3.output.clear();
        CHECK(Validate(c3, nvrhiOnly, 1, err) == Check::Invalid);
    }

    // LUID conversions
    {
        LUID l;
        l.LowPart = 0x12345678u; l.HighPart = 0x9;
        const uint64_t v = LuidToU64(l);
        CHECK(v == 0x912345678ull);
        uint8_t bytes[8];
        memcpy(bytes, &l, 8);
        CHECK(LuidFromBytes(bytes) == v);
        const LUID back = U64ToLuid(v);
        CHECK(back.LowPart == l.LowPart && back.HighPart == l.HighPart);
        const auto adapters = EnumerateAdapters();
        CHECK(!adapters.empty());
        if (!adapters.empty())
        {
            CHECK(FindDxgiAdapterIndex(adapters[0].luid) == 0);
            IDXGIAdapter1* a = FindDxgiAdapter(adapters[0].luid);
            CHECK(a != nullptr);
            if (a) a->Release();
        }
    }

    // scene hash
    {
        FakeSim sim;
        sim.s.resize(4); sim.d.resize(4);
        memset(sim.s.data(), 0, sizeof(FakeStatic) * 4);
        memset(sim.d.data(), 0, sizeof(FakeDynamic) * 4);
        sim.m.vertices = {1, 2, 3}; sim.m.indices = {0, 1, 2};
        const uint64_t h = StaticSceneHash(sim, 4), dh = DynamicSceneHash(sim, 4);
        CHECK(h == StaticSceneHash(sim, 4));
        sim.s[3].textureIndex = 1;
        CHECK(h != StaticSceneHash(sim, 4));
        sim.d[2].indexCount = 60;
        CHECK(dh != DynamicSceneHash(sim, 4));
    }

    // recorder + writers
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "benchmark_selftest_out";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        config = Config{};
        config.enabled = true; config.renderer = "nvrhi"; config.api = "d3d12"; config.binding = "tex_mut";
        config.threads = 1; config.warmup = 0.05; config.duration = 0.1; config.output = dir.string();
        config.gpuTiming = true;
        uint64_t frames = 0;
        while (recorder.BeginFrame())
        {
            { ScopedTimer t(recorder.Times().update); Sleep(1); }
            { ScopedTimer t(recorder.Times().render); Sleep(2); }
            recorder.SetFrameStats(50000, 123);
            if (recorder.FrameIndex() >= 2) recorder.SetGpuMs(recorder.FrameIndex() - 2, 0.5);
            ++frames;
        }
        CHECK(recorder.Done() && !recorder.Rows().empty() && recorder.WarmupFrames() > 0);
        CHECK(recorder.Rows().size() + recorder.WarmupFrames() == frames);
        const Row& r0 = recorder.Rows().front();
        CHECK(r0.frame == recorder.WarmupFrames() && r0.times.render >= 1.5 && r0.times.update >= 0.5);
        CHECK(r0.frameMs >= r0.times.render + r0.times.update && r0.times.gpu == 0.5 && r0.drawCount == 50000);

        RunInfo info;
        info.width = 1080; info.height = 720; info.threads = 1; info.asteroids = 50000; info.meshes = 1000; info.textures = 10;
        info.staticSceneHash = 0xabcdefull;
        info.extra.emplace_back("note", "a \"quoted\" \\ value");
        CHECK(Finish(info) == kExitOk);
        const std::string csv = ReadFile(dir / "frames.csv"), json = ReadFile(dir / "run.json");
        CHECK(csv.rfind(std::string(kCsvHeader) + "\n", 0) == 0);
        CHECK(size_t(std::count(csv.begin(), csv.end(), '\n')) == recorder.Rows().size() + 1);
        CHECK(json.find("\"status\": \"ok\"") != std::string::npos);
        CHECK(json.find("\"static_scene_hash\": \"0x0000000000abcdef\"") != std::string::npos);
        CHECK(json.find("\\\"quoted\\\" \\\\ value") != std::string::npos);
        CHECK(!std::filesystem::exists(dir / "run.json.tmp"));

        info.threads = 2; // mismatch with the request must fail, never be recorded as ok
        CHECK(Finish(info) == kExitFailed);
        CHECK(ReadFile(dir / "run.json").find("\"status\": \"failed\"") != std::string::npos);

        std::vector<uint8_t> px(4 * 4 * 4, 200);
        std::string err;
        CHECK(WriteCapture(px.data(), 4, 4, 16, true, err) && std::filesystem::file_size(dir / "capture.bmp") == 54 + 48);
        std::filesystem::remove_all(dir, ec);
    }

    CHECK(Median({3, 1, 2}) == 2 && Median({4, 1, 2, 3}) == 2.5);
    printf(gFailures ? "benchmark_selftest: %d FAILED\n" : "benchmark_selftest: ok\n", gFailures);
    return gFailures ? 1 : 0;
}

// SPDX-License-Identifier: Apache-2.0
// Public-only API fixture. Expected lookup scans explicit leaf boxes, not the GPU tree.
#include "GVDBNvrhiObstacleApi.h"
#include <ShaderToolArtifact.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <nvrhi/core/foundation.h>
#if ETHEREAL_TEST_CUDA
#include <nvrhi/cuda.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <vector>
using namespace nvrhi;
namespace g = gvdb::portable;
namespace {
std::atomic_uint problems{0};
void require(bool condition, const char* text) {
    if (!condition)
        throw std::runtime_error(text);
}
void check(FRESULT status, const char* text) {
    if (NVRHI_FAILED(status))
        throw std::runtime_error(std::string(text) + ": " + std::to_string(status));
}
struct Messages : ObjectImpl<IMessageCallback> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity severity, const char* text) noexcept override {
        if (severity >= MessageSeverity::Warning)
            ++problems;
        std::fprintf(stderr, "%s\n", text);
    }
};
struct Device {
    AutoPtr<donut::app::DeviceManager> manager;
    DeviceHandle device;
    AutoPtr<IMessageCallback> callback;
    Device(int argc, const char** argv) {
        bool useCuda = false, validation = false;
        int adapter = -1;
        for (int i = 1; i < argc; ++i) {
            useCuda |= !std::strcmp(argv[i], "-cuda");
            validation |= !std::strcmp(argv[i], "--validation");
            if (!std::strcmp(argv[i], "--adapter")) {
                require(++i < argc, "--adapter requires an ordinal");
                const std::string text = argv[i];
                size_t consumed = 0;
                adapter = std::stoi(text, &consumed);
                require(adapter >= 0 && consumed == text.size(), "invalid adapter ordinal");
            }
        }
        if (useCuda) {
#if ETHEREAL_TEST_CUDA
            cuda::DeviceDesc desc;
            desc.enableOptiX = false;
            desc.deviceOrdinal = adapter < 0 ? 0 : adapter;
            callback = MAKE_RC_OBJ_PTR(Messages);
            desc.errorCB = callback;
            device = cuda::createDevice(desc);
#else
            throw std::runtime_error("CUDA backend unavailable");
#endif
        } else {
            manager = TakeOver(
                donut::app::DeviceManager::Create(donut::app::GetGraphicsAPIFromCommandLine(argc, argv)));
            require(bool(manager), "create graphics manager");
            donut::app::DeviceCreationParameters desc;
            desc.adapterIndex = adapter;
            desc.enableNvrhiValidationLayer = true;
            desc.enableDebugRuntime = validation;
            require(manager->CreateHeadlessDevice(desc), "create graphics device");
            device = manager->GetDevice();
        }
        require(bool(device), "create test device");
        if (manager)
            std::printf("Obstacle backend %s; adapter %d (%s): %s\n",
                        donut::app::GetShaderTypeName(device->getGraphicsAPI()), adapter,
                        adapter < 0 ? "automatic" : "explicit", manager->GetRendererString());
        else
            std::printf("Obstacle backend CUDA; device ordinal %d\n", adapter < 0 ? 0 : adapter);
    }
    ~Device() {
        if (device)
            device->waitForIdle();
        device = nullptr;
        if (manager)
            manager->Shutdown();
    }
};
template <class T>
void queryIdentity(T* object) {
    const auto count = object->AddRef();
    object->Release();
    AutoPtr<T> same;
    check(object->QueryInterface(uuid_of<T>(), reinterpret_cast<void**>(same.GetAddressOf())),
          "self QueryInterface");
    require(same.Get() == object, "self identity");
    same = nullptr;
    AutoPtr<IObject> identity, again;
    check(object->QueryInterface(uuid_of<IObject>(), reinterpret_cast<void**>(identity.GetAddressOf())),
          "base identity");
    check(identity->QueryInterface(uuid_of<IObject>(), reinterpret_cast<void**>(again.GetAddressOf())),
          "repeat base identity");
    require(identity == again, "canonical IObject identity");
    identity = nullptr;
    again = nullptr;
    AutoPtr<IRHIObject> resource;
    check(object->QueryInterface(uuid_of<IRHIObject>(), reinterpret_cast<void**>(resource.GetAddressOf())),
          "RHI ancestor");
    resource = nullptr;
    void* missing = reinterpret_cast<void*>(uintptr_t(1));
    require(object->QueryInterface(uuid_of<IDevice>(), &missing) == FE_NOINTERFACE && !missing,
            "unsupported IID clears output");
    require(object->QueryInterface(uuid_of<T>(), nullptr) == FS_OK, "query-only IID probe");
    require(object->AddRef() == count, "queries preserve reference balance");
    object->Release();
}

struct Harness {
    IDevice* device;
    CommandListHandle commands;
    explicit Harness(IDevice* d)
        : device(d) {
        check(device->createCommandList({}, &commands), "create command list");
    }
    g::RecordingContext context() const { return {device, commands}; }
    void begin() { commands->open(); }
    void submit() {
        commands->close();
        const auto serial = device->executeCommandList(commands);
        require(serial || device->getGraphicsAPI() == GraphicsAPI::D3D11, "submit commands");
        require(device->waitForIdle(), "caller GPU completion");
        device->runGarbageCollection();
    }
    std::vector<uint8_t> read(IBuffer* buffer, uint64_t size) {
        if (!size)
            return {};
        BufferHandle staging;
        check(device->createBuffer(BufferDesc()
                                       .setByteSize(size)
                                       .setCpuAccess(CpuAccessMode::Read)
                                       .setInitialState(ResourceStates::CopyDest)
                                       .setKeepInitialState(true),
                                   &staging),
              "create test readback");
        begin();
        commands->copyBuffer(staging, 0, buffer, 0, size);
        submit();
        const void* data = device->mapBuffer(staging, CpuAccessMode::Read);
        require(data, "map canonical GPU stream");
        std::vector<uint8_t> result(size_t(size), uint8_t(0));
        std::memcpy(result.data(), data, result.size());
        device->unmapBuffer(staging);
        return result;
    }
};
std::vector<char> rewriteArtifact(
    const std::vector<char>& source,
    const std::function<void(ShaderToolArtifactEntry&, std::vector<ShaderToolArtifactBinding>&)>& modify) {
    ShaderToolArtifactView view;
    require(ShaderToolArtifactOpen(source.data(), source.size(), &view) == 0, "open accepted artifact");
    ShaderToolArtifactInfo info;
    ShaderToolArtifactEntry entry;
    require(ShaderToolArtifactGetInfo(&view, &info) == 0 && info.EntryCount == 1 &&
                ShaderToolArtifactGetEntry(&view, 0, &entry) == 0,
            "single entry artifact");
    std::vector<ShaderToolArtifactBinding> bindings(info.BindingCount);
    for (uint32_t i = 0; i < info.BindingCount; ++i)
        require(ShaderToolArtifactGetBinding(&view, i, &bindings[i]) == 0, "artifact reflection");
    modify(entry, bindings);
    ShaderToolArtifactDesc desc{};
    desc.Target = info.Target;
    desc.Flags = info.Flags;
    desc.Compiler = info.Compiler;
    desc.Entries = &entry;
    desc.EntryCount = 1;
    desc.Bindings = bindings.data();
    desc.BindingCount = uint32_t(bindings.size());
    size_t count = 0;
    require(ShaderToolArtifactWrite(&desc, nullptr, 0, &count) == 0, "size altered valid container");
    std::vector<char> result(count);
    require(ShaderToolArtifactWrite(&desc, result.data(), result.size(), &count) == 0,
            "write altered valid container");
    require(ShaderToolArtifactOpen(result.data(), result.size(), &view) == 0,
            "altered artifact structurally valid");
    return result;
}

struct Box {
    g::Int3 origin;
    uint32_t width, index;
    bool active;
};
struct Fixture {
    g::VolumeHandle owner;
    g::SnapshotHandle snapshot;
    std::vector<Box> boxes;
    uint32_t width = 0, pool = 0;
};
Fixture make(Harness& h, const std::vector<char>& remap, unsigned logWidth,
             const std::vector<g::Int3>& positions, int inactive = -1, bool root = false) {
    g::VolumeCreateDesc d;
    d.device = h.device;
    d.remapArtifact = remap.data();
    d.remapArtifactBytes = remap.size();
    d.configuration.levelCount = root ? 1 : 6;
    d.configuration.logDimension = {};
    d.configuration.logDimension[0] = uint8_t(logWidth);
    for (unsigned i = 1; i < d.configuration.levelCount; ++i)
        d.configuration.logDimension[i] = 2;
    Fixture f;
    f.width = 1u << logWidth;
    check(g::createVolume(d, &f.owner), "create topology owner");
    if (!root && !positions.empty()) {
        g::NodeId parent;
        check(f.owner->allocateNode(5, {-1024, -1024, -1024}, true, parent), "enclosing root");
        check(f.owner->setRoot(parent), "set enclosing root");
    }
    for (size_t i = 0; i < positions.size(); ++i) {
        g::NodeId node;
        if (root) {
            check(f.owner->allocateNode(0, positions[i], true, node), "root leaf");
            check(f.owner->setRoot(node), "set root");
        } else
            check(f.owner->activateAtLevel(0, positions[i], node), "activate fixture leaf");
        if (int(i) == inactive)
            check(f.owner->setActive(node, false), "inactive leaf");
        const auto* n = f.owner->getNode(node);
        f.boxes.push_back({n->position, f.width, uint32_t(node.index()), int(i) != inactive});
    }
    check(f.owner->finishTopology(), "finish fixture");
    f.pool = uint32_t(f.owner->getPoolCount(0, 0));
    g::GenerationDesc generation;
    generation.placement.minimumBrickCount = {1, 1, 1};
    g::GenerationTicketHandle ticket;
    check(f.owner->prepareGeneration(generation, &ticket), "prepare topology generation");
    h.begin();
    check(f.owner->recordAndPublish(h.context(), ticket), "publish topology");
    h.submit();
    check(f.owner->getSnapshot(&f.snapshot), "snapshot topology");
    return f;
}
g::ObstacleHandle factory(IDevice* device, const std::vector<char>& artifact) {
    g::ObstacleCreateDesc d;
    d.device = device;
    d.artifact = artifact.data();
    d.artifactBytes = artifact.size();
    g::ObstacleHandle op;
    check(g::createObstacle(d, &op), "create obstacle operator");
    return op;
}
std::vector<uint32_t> expected(const Fixture& s, const Fixture& o, uint32_t width,
                               const std::vector<uint32_t>* map, uint32_t bricks) {
    const uint32_t edge = s.width / width, n = edge * edge * edge;
    std::vector<uint32_t> out(size_t(bricks) * n, UINT32_MAX);
    for (const auto& a : s.boxes) {
        if (!a.active)
            continue;
        const uint32_t slot = map ? (*map)[a.index] : a.index;
        if (slot == UINT32_MAX)
            continue;
        uint32_t match = UINT32_MAX;
        for (const auto& b : o.boxes) {
            if (!b.active)
                continue;
            if (a.origin.x >= b.origin.x && int64_t(a.origin.x) < int64_t(b.origin.x) + b.width &&
                a.origin.y >= b.origin.y && int64_t(a.origin.y) < int64_t(b.origin.y) + b.width &&
                a.origin.z >= b.origin.z && int64_t(a.origin.z) < int64_t(b.origin.z) + b.width) {
                require(match == UINT32_MAX, "nonoverlapping reference boxes");
                match = b.index;
            }
        }
        std::fill_n(out.begin() + size_t(slot) * n, n, match);
    }
    return out;
}
void compare(g::BorrowedBytes bytes, const std::vector<uint32_t>& ref) {
    require(bytes.byteCount == ref.size() * 4, "exact output size");
    require(ref.empty() ? bytes.data == nullptr
                        : bytes.data && std::memcmp(bytes.data, ref.data(), ref.size() * 4) == 0,
            "independent geometric leaf reference");
}
void exercise(Harness& h, g::ObstacleHandle op, const Fixture& s, const Fixture& o, uint32_t width,
              const std::vector<uint32_t>* mapping = nullptr, uint32_t bricks = 0) {
    auto copy = mapping ? *mapping : std::vector<uint32_t>{};
    uint32_t emptySentinel = UINT32_MAX;
    g::ObstacleRequestDesc d;
    d.source = s.snapshot;
    d.obstacle = o.snapshot;
    d.subcellWidth = width;
    if (mapping) {
        d.sourceToOutput = copy.empty() ? &emptySentinel : copy.data();
        d.mappingCount = uint32_t(copy.size());
        d.outputBrickCount = bricks;
    } else
        bricks = s.pool;
    auto ref = expected(s, o, width, mapping, bricks);
    g::ObstacleTicketHandle t;
    const uint64_t host = uint64_t(bricks) * 4 + ref.size() * 4,
                   gpu = ref.empty() ? 0 : 384 + uint64_t(bricks) * 4 + ref.size() * 8;
    g::ObstaclePreparationBudget budget;
    budget.maxHostBytes = host;
    budget.maxAllocatedBytes = gpu;
    budget.maxBuffers = ref.empty() ? 0 : 6;
    check(op->prepare(d, budget, &t), "prepare exact-budget mapping");
    queryIdentity(t.Get());
    require(t->getDevice() == h.device && t->getOutputBrickCount() == bricks &&
                t->getSubcellsPerBrick() == (s.width / width) * (s.width / width) * (s.width / width),
            "ticket metadata");
    auto reject = [&](g::ObstaclePreparationBudget b) {
        auto* out = reinterpret_cast<g::IGVDBObstacleTicket*>(uintptr_t(1));
        require(op->prepare(d, b, &out) == FE_OUT_OF_MEMORY && !out, "budget rejection clears output");
    };
    if (host) {
        auto b = budget;
        --b.maxHostBytes;
        reject(b);
    }
    if (gpu) {
        auto b = budget;
        --b.maxAllocatedBytes;
        reject(b);
        b = budget;
        --b.maxBuffers;
        reject(b);
    }
    g::BorrowedBytes bytes{reinterpret_cast<const void*>(uintptr_t(1)), 77};
    require(NVRHI_FAILED(t->finishAfterCallerCompletion(bytes)) && bytes.byteCount == 77,
            "early finish preserves output");
    require(NVRHI_FAILED(t->getValues(bytes)) && bytes.byteCount == 77, "early values preserve output");
    g::BorrowedBufferBinding binding;
    binding.byteSize = 99;
    require(NVRHI_FAILED(t->getOutputBuffer(binding)) && binding.byteSize == 99,
            "early GPU output preserves binding");
    std::fill(copy.begin(), copy.end(), UINT32_MAX);  // prepare copied caller memory.
    require(NVRHI_FAILED(op->record({}, t)), "invalid context preserves recordability");
    h.begin();
    check(op->record(h.context(), t), "record mapping");
    require(NVRHI_FAILED(op->record(h.context(), t)), "record once");
    h.submit();
    check(t->getOutputBuffer(binding), "GPU output");
    require(binding.byteOffset == 0 && binding.byteSize == ref.size() * 4, "GPU range");
    auto gpuBytes = h.read(binding.buffer, binding.byteSize);
    require(ref.empty() ? gpuBytes.empty() : std::memcmp(gpuBytes.data(), ref.data(), ref.size() * 4) == 0,
            "GPU mapping reference");
    check(t->finishAfterCallerCompletion(bytes), "finish mapping");
    compare(bytes, ref);
    check(t->getValues(bytes), "repeat values");
    compare(bytes, ref);
    require(NVRHI_FAILED(t->finishAfterCallerCompletion(bytes)), "finish once");
    compare(bytes, ref);
}
class ForeignSnapshot final : public ObjectImpl<g::IGVDBSnapshot> {
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ForeignSnapshot)
    NVRHI_IMPLEMENTS_INTERFACE(g::IGVDBSnapshot)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    IDevice* getDevice() const noexcept override { return nullptr; }
    g::SnapshotDesc getDesc() const noexcept override { return {}; }
    FRESULT getLevel(uint32_t, g::LevelInfo&) const noexcept override { return FE_UNSUPPORTED; }
    FRESULT getTopologyBuffer(g::TopologyBuffer, g::BorrowedBufferBinding&) const noexcept override {
        return FE_UNSUPPORTED;
    }
    FRESULT getChannel(uint32_t, g::ChannelSnapshotDesc&) const noexcept override { return FE_UNSUPPORTED; }
};
void run(IDevice* device, const std::vector<char>& remap, const std::vector<char>& artifact) {
    Harness h(device);
    auto op = factory(device, artifact);
    queryIdentity(op.Get());
    auto source = make(h, remap, 1,
                       {
                           {-10,   -2,    -2   },
                           {-8,    0,     0    },
                           {-6,    0,     0    },
                           {-2,    0,     0    },
                           {0,     0,     0    },
                           {2,     0,     0    },
                           {4,     0,     0    },
                           {8,     0,     0    },
                           {12,    0,     0    },
                           {0,     -2,    0    },
                           {0,     0,     -2   },
                           {0,     4,     0    },
                           {0,     0,     4    },
                           {1000,  1000,  1000 },
                           {-1000, -1000, -1000}
    },
                       5);
    auto obstacle = make(h, remap, 2,
                         {
                             {-8,    0,     0    },
                             {0,     0,     0    },
                             {4,     0,     0    },
                             {1000,  1000,  1000 },
                             {-1000, -1000, -1000}
    },
                         2);
    for (uint32_t width : {1u, 2u})
        exercise(h, op, source, obstacle, width);
    std::vector<uint32_t> mapping(source.pool);
    for (uint32_t i = 0; i < source.pool; ++i)
        mapping[i] = (source.pool - i) * 2;
    mapping[1] = UINT32_MAX;
    exercise(h, op, source, obstacle, 1, &mapping, source.pool * 2 + 3);
    auto root = make(h, remap, 2,
                     {
                         {0, 0, 0}
    },
                     -1, true);
    exercise(h, op, source, root, 2);
    exercise(h, op, root, root, 1);
    exercise(h, op, root, root, 4);
    auto negativeRoot = make(h, remap, 2,
                             {
                                 {-8, 0, 0}
    },
                             -1, true);
    exercise(h, op, source, negativeRoot, 1);
    exercise(h, op, root, source, 1);  // Different leaf widths; only source origin is queried.
    auto empty = make(h, remap, 1, {});
    exercise(h, op, source, empty, 1);
    exercise(h, op, empty, source, 1);
    exercise(h, op, empty, empty, 1);
    std::vector<uint32_t> emptyMap;
    exercise(h, op, empty, source, 1, &emptyMap, 3);
    std::vector<uint32_t> excluded(source.pool, UINT32_MAX);
    exercise(h, op, source, obstacle, 1, &excluded, 0);
    auto inactiveRoot = make(h, remap, 2,
                             {
                                 {0, 0, 0}
    },
                             0, true);
    exercise(h, op, inactiveRoot, root, 1);
    g::ObstacleRequestDesc d;
    d.source = source.snapshot;
    d.obstacle = obstacle.snapshot;
    auto reject = [&](g::ObstacleRequestDesc bad) {
        auto* out = reinterpret_cast<g::IGVDBObstacleTicket*>(uintptr_t(1));
        require(NVRHI_FAILED(op->prepare(bad, {}, &out)) && !out, "invalid request clears ticket");
    };
    for (unsigned i = 0; i < 9; ++i) {
        auto bad = d;
        switch (i) {
            case 0:
                bad.structSize--;
                break;
            case 1:
                bad.reserved[1] = 1;
                break;
            case 2:
                bad.subcellWidth = 0;
                break;
            case 3:
                bad.subcellWidth = 3;
                break;
            case 4:
                bad.subcellWidth = 4;
                break;
            case 5:
                bad.source = nullptr;
                break;
            case 6:
                bad.obstacle = nullptr;
                break;
            case 7:
                bad.mappingCount = 1;
                break;
            case 8:
                bad.outputBrickCount = 1;
                break;
        }
        reject(bad);
    }
    auto foreign = MAKE_RC_OBJ_PTR(ForeignSnapshot);
    auto bad = d;
    bad.source = foreign;
    reject(bad);
    bad = d;
    bad.obstacle = foreign;
    reject(bad);
    auto invalidMap = mapping;
    invalidMap[0] = invalidMap[2];
    bad = d;
    bad.sourceToOutput = invalidMap.data();
    bad.mappingCount = source.pool;
    bad.outputBrickCount = source.pool * 2 + 3;
    reject(bad);
    invalidMap[0] = bad.outputBrickCount;
    reject(bad);
    invalidMap[0] = 0;
    --bad.mappingCount;
    reject(bad);
    bad.mappingCount = source.pool;
    bad.outputBrickCount = UINT32_MAX;
    reject(bad);
    g::ObstaclePreparationBudget invalidBudget;
    invalidBudget.reserved = 1;
    g::ObstacleTicketHandle t;
    require(NVRHI_FAILED(op->prepare(d, invalidBudget, &t)) && !t, "reserved budget");
    require(op->prepare(d, {}, nullptr) == FE_INVALID_ARGS, "null ticket output");
    check(op->prepare(d, {}, &t), "retained preparation");
    auto other = factory(device, artifact);
    h.begin();
    require(NVRHI_FAILED(other->record(h.context(), t)), "operator identity");
    h.submit();
    auto reference = expected(source, obstacle, 1, nullptr, source.pool);
    check(source.owner->clearTopology(), "change source after preparation");
    check(source.owner->finishTopology(), "finish changed host");
    source.owner = nullptr;
    obstacle.owner = nullptr;
    source.snapshot = nullptr;
    obstacle.snapshot = nullptr;
    h.begin();
    check(op->record(h.context(), t), "old snapshots survive host and owner release");
    h.submit();
    op = nullptr;
    g::BorrowedBytes bytes;
    check(t->finishAfterCallerCompletion(bytes), "finish after operator release");
    compare(bytes, reference);
    auto rejectArtifact = [&](const std::vector<char>& a) {
        g::ObstacleCreateDesc c;
        c.device = device;
        c.artifact = a.data();
        c.artifactBytes = a.size();
        auto* out = reinterpret_cast<g::IGVDBObstacle*>(uintptr_t(1));
        require(NVRHI_FAILED(g::createObstacle(c, &out)) && !out, "artifact contract rejection");
    };
    rejectArtifact({});
    rejectArtifact({'b', 'a', 'd'});
    rejectArtifact(remap);
    rejectArtifact(rewriteArtifact(artifact, [](auto& e, auto&) { e.ThreadGroupSize[0] = 128; }));
    rejectArtifact(rewriteArtifact(artifact, [](auto& e, auto&) { e.Name = e.Symbol = "wrong"; }));
    rejectArtifact(rewriteArtifact(artifact, [](auto&, auto& b) { b[0].ElementStride += 16; }));
    rejectArtifact(rewriteArtifact(artifact, [](auto&, auto& b) {
        for (auto& x : b)
            if (x.Scope == ST_SCOPE_CONSTANT_BUFFER) {
                x.Offset += 4;
                break;
            }
    }));
    std::puts(
        "PASS obstacle signed/deep/root/empty/inactive/different-width geometry, copied permutations/holes, "
        "GPU/CPU results, exact budgets, lifetime, invalid inputs and artifact contracts");
}
std::vector<char> artifact(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "open obstacle artifact");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}  // namespace
int main(int argc, const char** argv) {
    donut::log::ConsoleApplicationMode();
    donut::log::SetCallback([](donut::log::Severity severity, const char* text) {
        if (severity >= donut::log::Severity::Warning)
            ++problems;
        std::fprintf(stderr, "%s\n", text);
    });
    try {
        {
            Device device(argc, argv);
            const auto target = donut::app::GetShaderTypeName(device.device->getGraphicsAPI());
            const auto base = donut::app::GetDirectoryWithExecutable() / "shaders";
            run(device.device, artifact(base / "gvdb_owner" / target / "GVDBAtlasRepack_relocateAtlas.bin"),
                artifact(base / "gvdb_obstacle" / target / "GVDBObstacleMapping_mapObstacleSubcells.bin"));
        }
        {
            Device first(argc, argv);
            Device second(argc, argv);
            const auto target = donut::app::GetShaderTypeName(first.device->getGraphicsAPI());
            const auto base = donut::app::GetDirectoryWithExecutable() / "shaders";
            const auto remap = artifact(base / "gvdb_owner" / target / "GVDBAtlasRepack_relocateAtlas.bin");
            const auto code =
                artifact(base / "gvdb_obstacle" / target / "GVDBObstacleMapping_mapObstacleSubcells.bin");
            Harness h1(first.device), h2(second.device);
            auto a = make(h1, remap, 1,
                          {
                              {0, 0, 0}
            },
                          -1, true);
            auto b = make(h2, remap, 1,
                          {
                              {0, 0, 0}
            },
                          -1, true);
            auto op = factory(first.device, code);
            g::ObstacleRequestDesc request;
            request.source = a.snapshot;
            request.obstacle = b.snapshot;
            g::ObstacleTicketHandle t;
            require(NVRHI_FAILED(op->prepare(request, {}, &t)) && !t, "foreign obstacle device");
            request.source = b.snapshot;
            request.obstacle = a.snapshot;
            require(NVRHI_FAILED(op->prepare(request, {}, &t)) && !t, "foreign source device");
            request.source = a.snapshot;
            check(op->prepare(request, {}, &t), "same-device query");
            h2.begin();
            require(NVRHI_FAILED(op->record(h2.context(), t)), "foreign command device");
            h2.submit();
            h1.begin();
            check(op->record(h1.context(), t), "valid record after foreign device rejection");
            h1.submit();
            g::BorrowedBytes bytes;
            check(t->finishAfterCallerCompletion(bytes), "finish after foreign rejection");
            std::puts("PASS independent device rejection");
        }
        require(problems == 0, "validation warnings/errors");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}

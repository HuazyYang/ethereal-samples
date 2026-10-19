// SPDX-License-Identifier: Apache-2.0
// Public-only API fixture. Expected lookup scans explicit leaf boxes, not the GPU tree.
#include "GVDBNvrhiGridQueryApi.h"
#include <cmath>
#include <limits>
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
            std::printf("Grid query backend %s; adapter %d (%s): %s\n",
                        donut::app::GetShaderTypeName(device->getGraphicsAPI()), adapter,
                        adapter < 0 ? "automatic" : "explicit", manager->GetRendererString());
        else
            std::printf("Grid query backend CUDA; device ordinal %d\n", adapter < 0 ? 0 : adapter);
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
    check(f.owner->setChannel(0, {g::AtlasFormat::R32_FLOAT, {}}), "scalar channel");
    check(f.owner->setChannel(31,
                              {
                                  g::AtlasFormat::R32_FLOAT, {7, 0, 0, 0}
    }),
          "high scalar channel");
    check(f.owner->setChannel(1, {g::AtlasFormat::RGBA8_UINT, {}}), "unsupported query channel");
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
std::vector<char> artifact(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "open grid query fixture artifact");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
g::GridQueryHandle factory(IDevice* device, const std::vector<char>& code) {
    g::GridQueryCreateDesc d;
    d.device = device;
    d.artifact = code.data();
    d.artifactBytes = code.size();
    g::GridQueryHandle op;
    check(g::createGridQuery(d, &op), "create grid query");
    return op;
}
void run(IDevice* device, uint32_t count, unsigned components, bool root, bool emptyTopology, unsigned apron,
         unsigned pattern, bool largeOrigin = false) {
    Harness h(device);
    const auto base = donut::app::GetDirectoryWithExecutable() / "shaders";
    const auto target = donut::app::GetShaderTypeName(device->getGraphicsAPI());
    const auto remap = artifact(base / "gvdb_point_cloud" / target / "GVDBAtlasRepack_relocateAtlas.bin");
    const auto conversion =
        artifact(base / "gvdb_point_cloud" / target / "GVDBPointStreamConversion_convertPointStream.bin");
    const auto code = artifact(base / "gvdb_grid_query" / target / "GVDBGridQuery_queryScalarGrid.bin");
    const g::Int3 rootOrigin = largeOrigin ? g::Int3{8388599, 5, -8388599} : g::Int3{-3, 5, -2};
    auto f=make(h,remap,2,emptyTopology?std::vector<g::Int3>{}:root?std::vector<g::Int3>{rootOrigin}:std::vector<g::Int3>{{-8,0,0},{0,0,0},{8,0,0}},root?-1:1,root);
    // Publish the requested apron, then upload an independent full-atlas pattern.
    g::GenerationDesc generation;
    generation.placement.minimumBrickCount = {1, 1, 1};
    generation.placement.apron = apron;
    g::GenerationTicketHandle gen;
    check(f.owner->prepareGeneration(generation, &gen), "prepare apron");
    h.begin();
    check(f.owner->recordAndPublish(h.context(), gen), "publish apron");
    h.submit();
    f.snapshot = nullptr;
    check(f.owner->getSnapshot(&f.snapshot), "apron snapshot");
    gen = nullptr;
    auto res = f.snapshot->getDesc().volume.atlasResolutionAndApron;
    std::vector<float> atlas(size_t(res.x) * res.y * res.z);
    for (unsigned z = 0; z < res.z; ++z)
        for (unsigned y = 0; y < res.y; ++y)
            for (unsigned x = 0; x < res.x; ++x)
                atlas[(z * res.y + y) * res.x + x] =
                    pattern == 0 ? 3.f
                    : pattern == 1
                        ? float(x) + 2.f * y - 3.f * z
                        : float(int((x * 13 + y * 7 + z * 23) % 31) - 15) * .125f + float(x * x) * .0625f;
    if (pattern == 3)
        for (size_t i = 0; i < atlas.size(); ++i) {
            if (i % 11 == 0)
                atlas[i] = std::numeric_limits<float>::quiet_NaN();
            else if (i % 17 == 0)
                atlas[i] = std::numeric_limits<float>::infinity();
            else if (i % 19 == 0)
                atlas[i] = -std::numeric_limits<float>::infinity();
        }
    if (!atlas.empty()) {
        g::ChannelUploadDesc data{0, 0, atlas.data(), atlas.size() * 4};
        generation.uploads = &data;
        generation.uploadCount = 1;
        check(f.owner->prepareGeneration(generation, &gen), "prepare atlas pattern");
        h.begin();
        check(f.owner->recordAndPublish(h.context(), gen), "publish atlas pattern");
        h.submit();
        f.snapshot = nullptr;
        check(f.owner->getSnapshot(&f.snapshot), "pattern snapshot");
        gen = nullptr;
    }
    // Generation publication spatially rebuilds aprons. The sampling oracle uses
    // the immutable input bytes (not the output of the query under test).
    g::ChannelSnapshotDesc channel;
    check(f.snapshot->getChannel(0, channel), "scalar input");
    auto scalarBytes = h.read(channel.raw.buffer, channel.raw.byteOffset + channel.raw.byteSize);
    if (!atlas.empty())
        std::memcpy(atlas.data(), scalarBytes.data() + channel.raw.byteOffset, atlas.size() * 4);
    check(f.snapshot->getChannel(31, channel), "constant input");
    auto constantBytes = h.read(channel.raw.buffer, channel.raw.byteOffset + channel.raw.byteSize);
    std::vector<float> constantAtlas(atlas.size());
    if (!atlas.empty())
        std::memcpy(constantAtlas.data(), constantBytes.data() + channel.raw.byteOffset, atlas.size() * 4);
    g::BorrowedBufferBinding nodeBuffer;
    check(f.snapshot->getTopologyBuffer(g::TopologyBuffer::Nodes, nodeBuffer), "node bytes");
    auto nodes = h.read(nodeBuffer.buffer, nodeBuffer.byteOffset + nodeBuffer.byteSize);
    g::LevelInfo leafLevel;
    check(f.snapshot->getLevel(0, leafLevel), "leaf metadata");
    std::vector<g::Int3> atlasOrigins(f.pool);
    for (const auto& box : f.boxes)
        std::memcpy(
            &atlasOrigins[box.index],
            nodes.data() + nodeBuffer.byteOffset + leafLevel.nodes.x + box.index * leafLevel.nodes.y + 16, 12);
    g::PointOpsCreateDesc pc;
    pc.volume = f.owner;
    pc.conversionArtifact = conversion.data();
    pc.conversionArtifactBytes = conversion.size();
    g::PointOpsHandle pointOp;
    check(g::createPointOps(pc, &pointOp), "point factory");
    std::vector<g::Float4> positions(count);
    const g::Int3 origin = root ? rootOrigin : g::Int3{-8, 0, 0};
    for (unsigned i = 0; i < count; ++i) {
        positions[i] = {origin.x + float(i % 41) * .25f - 1, origin.y + float((i / 3) % 23) * .25f - 1,
                        origin.z + float((i / 7) % 21) * .25f - 1, 123};
        if (i % 43 == 0)
            positions[i].x = std::numeric_limits<float>::max();
        if (i % 47 == 0)
            positions[i].x = -2147483648.f;
        if (i % 53 == 0)
            positions[i].x = 2147483648.f;
        if (i == 1)
            positions[i] = {float(origin.x), float(origin.y), float(origin.z), 0};
        if (i == 2)
            positions[i] = {float(origin.x) + 3.75f, float(origin.y) + 3.75f, float(origin.z) + 3.75f, 0};
        if (i == 3)
            positions[i] = {float(origin.x) + .5f, float(origin.y), float(origin.z) + .5f, 0};
        if (i == 4)
            positions[i] = {float(origin.x) - .25f, float(origin.y), float(origin.z), 0};
        if (i == 5)
            positions[i] = {0, 0, 0, 0};  // Inactive slot in the multi-leaf case.
    }
    g::PointSetDesc input;
    input.positions.stream = {positions.data(), positions.size() * 16, 0, 16, count, g::PointComponent::Float32,
                              components};
    g::PointUploadTicketHandle upload;
    check(pointOp->preparePoints(input, {}, &upload), "prepare points");
    h.begin();
    check(pointOp->recordPoints(h.context(), upload), "record points");
    h.submit();
    g::PointSetHandle points;
    check(upload->finishAfterCallerCompletion(&points), "finish points");
    upload = nullptr;
    pointOp = nullptr;
    auto op = factory(device, code);
    queryIdentity(op.Get());
    g::GridQueryRequestDesc request;
    request.snapshot = f.snapshot;
    request.points = points;
    g::GridQueryPreparationBudget budget;
    budget.maxHostBytes = count * 16ull;
    budget.maxAllocatedBytes = count ? 384 + count * 32ull : 0;
    budget.maxBuffers = count ? 5 : 0;
    auto reject = [&](const g::GridQueryRequestDesc& bad, const g::GridQueryPreparationBudget& b,
                      FRESULT status) {
        auto* out = reinterpret_cast<g::IGVDBGridQueryTicket*>(uintptr_t(1));
        require(op->prepare(bad, b, &out) == status && !out, "query rejection clears output");
    };
    if (count) {
        auto b = budget;
        --b.maxHostBytes;
        reject(request, b, FE_OUT_OF_MEMORY);
        b = budget;
        --b.maxAllocatedBytes;
        reject(request, b, FE_OUT_OF_MEMORY);
        b = budget;
        --b.maxBuffers;
        reject(request, b, FE_OUT_OF_MEMORY);
    }
    auto bad = request;
    --bad.structSize;
    reject(bad, {}, FE_INVALID_ARGS);
    bad = request;
    bad.reserved = 1;
    reject(bad, {}, FE_INVALID_ARGS);
    bad = request;
    bad.channel = 32;
    reject(bad, {}, FE_INVALID_ARGS);
    bad = request;
    bad.channel = 2;
    reject(bad, {}, FE_NOT_FOUND);
    bad = request;
    bad.channel = 1;
    reject(bad, {}, FE_UNSUPPORTED);
    bad = request;
    bad.mode = g::GridQueryMode(2);
    reject(bad, {}, FE_INVALID_ARGS);
    bad = request;
    bad.snapshot = nullptr;
    reject(bad, {}, FE_INVALID_ARGS);
    bad = request;
    bad.points = nullptr;
    reject(bad, {}, FE_INVALID_ARGS);
    auto badBudget = budget;
    badBudget.reserved = 1;
    reject(request, badBudget, FE_INVALID_ARGS);
    require(op->prepare(request, {}, nullptr) == FE_INVALID_ARGS, "null ticket output");
    std::array<g::GridQueryTicketHandle, 4> tickets;
    for (unsigned mode = 0; mode < 4; ++mode) {
        request.mode = g::GridQueryMode(mode % 2);
        request.channel = mode < 2 ? 0 : 31;
        check(op->prepare(request, budget, &tickets[mode]), "prepare exact-budget query");
        require(tickets[mode]->getCount() == count && tickets[mode]->getDevice() == device, "ticket metadata");
        queryIdentity(tickets[mode].Get());
    }
    auto other = factory(device, code);
    g::BorrowedBytes untouched{reinterpret_cast<void*>(uintptr_t(1)), 123};
    require(NVRHI_FAILED(tickets[0]->getValues(untouched)) && untouched.byteCount == 123,
            "early values preserve output");
    require(NVRHI_FAILED(tickets[0]->finishAfterCallerCompletion(untouched)) && untouched.byteCount == 123,
            "early finish preserves output");
    g::BorrowedBufferBinding early{nullptr, 13, 17};
    require(NVRHI_FAILED(tickets[0]->getOutputBuffer(early)) && early.byteSize == 17,
            "early buffer preserves output");
    // Drop all original owners before record. Tickets retain immutable inputs.
    points = nullptr;
    f.snapshot = nullptr;
    f.owner = nullptr;
    h.begin();
    require(NVRHI_FAILED(other->record(h.context(), tickets[0])), "wrong factory");
    require(NVRHI_FAILED(op->record({}, tickets[0])), "missing context");
    for (auto& ticket : tickets) {
        check(op->record(h.context(), ticket), "record query");
        require(NVRHI_FAILED(op->record(h.context(), ticket)), "single record");
    }
    op = nullptr;
    other = nullptr;
    h.submit();
    auto sample = [&](int x, int y, int z, bool constant) -> double {
        if (x < 0 || y < 0 || z < 0 || unsigned(x) >= res.x || unsigned(y) >= res.y || unsigned(z) >= res.z)
            return 0;
        return (constant ? constantAtlas : atlas)[(size_t(z) * res.y + y) * res.x + x];
    };
    for (unsigned mode = 0; mode < 4; ++mode) {
        auto& ticket = tickets[mode];
        g::BorrowedBytes bytes;
        check(ticket->finishAfterCallerCompletion(bytes), "finish query");
        require(bytes.byteCount == count * 16ull && (count || bytes.data == nullptr), "result size");
        const auto* result = static_cast<const g::Float4*>(bytes.data);
        for (unsigned i = 0; i < count; ++i) {
            const auto p = positions[i];
            g::Float4 expected{};
            for (const auto& box : f.boxes) {
                if (!box.active || double(p.x) < box.origin.x || double(p.y) < box.origin.y ||
                    double(p.z) < box.origin.z || double(p.x) >= double(box.origin.x) + box.width ||
                    double(p.y) >= double(box.origin.y) + box.width ||
                    double(p.z) >= double(box.origin.z) + box.width)
                    continue;
                const auto a = atlasOrigins[box.index];
                const double x = double(p.x) - box.origin.x + (mode % 2 ? -.5 : 0),
                             y = double(p.y) - box.origin.y,
                             z = double(p.z) - box.origin.z + (mode % 2 ? -.5 : 0);
                const int ix = int(std::floor(x)), iy = int(std::floor(y)), iz = int(std::floor(z));
                double value = sample(a.x + ix, a.y + iy, a.z + iz, mode >= 2);
                expected.z = 1;
                if (mode % 2) {
                    expected.y = float(value);
                    value = 0;
                    const double tx = x - ix, ty = y - iy, tz = z - iz;
                    for (int dz = 0; dz < 2; ++dz)
                        for (int dy = 0; dy < 2; ++dy)
                            for (int dx = 0; dx < 2; ++dx)
                                value += sample(a.x + ix + dx, a.y + iy + dy, a.z + iz + dz, mode >= 2) *
                                         (dx ? tx : 1 - tx) * (dy ? ty : 1 - ty) * (dz ? tz : 1 - tz);
                }
                expected.x = float(value);
                break;
            }
            auto matches = [](float a, float b) {
                return std::isnan(b) ? std::isnan(a)
                       : std::isinf(b)
                           ? a == b
                           : std::isfinite(a) && std::abs(a - b) <= 2e-5f * std::max(1.f, std::abs(b));
            };
            if(!matches(result[i].x,expected.x) || !matches(result[i].y,expected.y) || result[i].z!=expected.z)
                std::fprintf(stderr,"pattern %u mode %u point %u (%g,%g,%g): actual (%g,%g,%g), expected (%g,%g,%g)\n",
                    pattern,mode,i,p.x,p.y,p.z,result[i].x,result[i].y,result[i].z,expected.x,expected.y,expected.z);
            require(matches(result[i].x, expected.x) && matches(result[i].y, expected.y) &&
                        result[i].z == expected.z && result[i].w == 0,
                    "independent scalar/MAC reference");
        }
        g::BorrowedBufferBinding gpu;
        check(ticket->getOutputBuffer(gpu), "result buffer");
        auto actual = h.read(gpu.buffer, gpu.byteSize);
        require(gpu.byteSize == bytes.byteCount &&
                    (!count || !std::memcmp(actual.data(), bytes.data, actual.size())),
                "actual GPU/CPU coherence");
        g::BorrowedBytes again;
        check(ticket->getValues(again), "repeat immutable borrow");
        require(again.data == bytes.data, "stable borrow");
        require(NVRHI_FAILED(ticket->finishAfterCallerCompletion(untouched)) && untouched.byteCount == 123,
                "one finish");
    }
    if (root && count) {
        auto rejectArtifact = [&](const std::vector<char>& bytes) {
            g::GridQueryCreateDesc d;
            d.device = device;
            d.artifact = bytes.data();
            d.artifactBytes = bytes.size();
            auto* out = reinterpret_cast<g::IGVDBGridQuery*>(uintptr_t(1));
            require(NVRHI_FAILED(g::createGridQuery(d, &out)) && !out, "malformed query artifact");
        };
        rejectArtifact({});
        rejectArtifact({'b', 'a', 'd'});
        rejectArtifact(remap);
        rejectArtifact(rewriteArtifact(code, [](auto& e, auto&) { e.ThreadGroupSize[0] = 128; }));
        rejectArtifact(rewriteArtifact(code, [](auto& e, auto&) { e.Name = e.Symbol = "wrongEntry"; }));
        rejectArtifact(rewriteArtifact(code, [](auto&, auto& bindings) {
            for (auto& b : bindings)
                if (b.Scope == ST_SCOPE_CONSTANT_BUFFER) {
                    b.Offset += 4;
                    break;
                }
        }));
        rejectArtifact(rewriteArtifact(code, [](auto&, auto& bindings) { bindings[0].ElementStride += 16; }));
    }
}
void foreignDevices(int argc, const char** argv) {
    Device first(argc, argv), second(argc, argv);
    Harness h1(first.device), h2(second.device);
    const auto base = donut::app::GetDirectoryWithExecutable() / "shaders";
    const auto target = donut::app::GetShaderTypeName(first.device->getGraphicsAPI());
    const auto remap = artifact(base / "gvdb_point_cloud" / target / "GVDBAtlasRepack_relocateAtlas.bin");
    const auto conversion =
        artifact(base / "gvdb_point_cloud" / target / "GVDBPointStreamConversion_convertPointStream.bin");
    const auto code = artifact(base / "gvdb_grid_query" / target / "GVDBGridQuery_queryScalarGrid.bin");
    auto a = make(h1, remap, 2,
                  {
                      {0, 0, 0}
    },
                  -1, true),
         b = make(h2, remap, 2, {{0, 0, 0}}, -1, true);
    auto emptyPoints = [&](Harness& h, g::IGVDBVolume* owner) {
        g::PointOpsCreateDesc d;
        d.volume = owner;
        d.conversionArtifact = conversion.data();
        d.conversionArtifactBytes = conversion.size();
        g::PointOpsHandle op;
        check(g::createPointOps(d, &op), "foreign point factory");
        g::PointUploadTicketHandle t;
        check(op->preparePoints({}, {}, &t), "empty points prepare");
        h.begin();
        check(op->recordPoints(h.context(), t), "empty points record");
        h.submit();
        g::PointSetHandle p;
        check(t->finishAfterCallerCompletion(&p), "empty points finish");
        return p;
    };
    auto pa = emptyPoints(h1, a.owner), pb = emptyPoints(h2, b.owner);
    auto op = factory(first.device, code);
    g::GridQueryRequestDesc d;
    d.snapshot = a.snapshot;
    d.points = pb;
    g::GridQueryTicketHandle t;
    require(NVRHI_FAILED(op->prepare(d, {}, &t)) && !t, "foreign points device");
    d.points = pa;
    d.snapshot = b.snapshot;
    require(NVRHI_FAILED(op->prepare(d, {}, &t)) && !t, "foreign snapshot device");
    d.snapshot = a.snapshot;
    check(op->prepare(d, {}, &t), "same device query");
    h2.begin();
    require(NVRHI_FAILED(op->record(h2.context(), t)), "foreign command list");
    h2.submit();
    h1.begin();
    check(op->record(h1.context(), t), "record after rejection");
    h1.submit();
    g::BorrowedBytes result;
    check(t->finishAfterCallerCompletion(result), "finish after rejection");
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
            for (unsigned components : {3u, 4u})
                for (unsigned apron : {0u, 1u, 2u})
                    run(device.device, 258, components, true, false, apron, apron);
            run(device.device, 513, 3, false, false, 1, 2);
            run(device.device, 258, 4, false, true, 1, 2);
            run(device.device, 0, 3, true, false, 1, 0);
            run(device.device, 0, 4, false, true, 0, 0);
            run(device.device, 1, 3, true, false, 1, 0);
            run(device.device, 258, 4, true, false, 1, 3);
            run(device.device, 258, 3, true, false, 1, 2, true);
        }
        foreignDevices(argc, argv);
        require(problems == 0, "validation warnings/errors");
        std::puts(
            "PASS grid queries: independent scalar/MAC reference, signed topology, empty/inactive/missing "
            "leaves, stored aprons/border, exact budgets, retained lifetime, artifacts and GPU/CPU coherence");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}

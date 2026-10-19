// SPDX-License-Identifier: Apache-2.0
// Public-only API fixture. Expected lookup scans explicit leaf boxes, not the GPU tree.
#include "GVDBNvrhiPointInsertionApi.h"
#include "GVDBNvrhiPointQuantizeApi.h"
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
            std::printf("Insertion backend %s; adapter %d (%s): %s\n",
                        donut::app::GetShaderTypeName(device->getGraphicsAPI()), adapter,
                        adapter < 0 ? "automatic" : "explicit", manager->GetRendererString());
        else
            std::printf("Insertion backend CUDA; device ordinal %d\n", adapter < 0 ? 0 : adapter);
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
std::vector<char> artifact(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "open subcell fixture artifact");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
g::PointInsertionHandle factory(IDevice* device, const std::vector<char>& artifact) {
    g::SubcellCreateDesc d;
    d.device = device;
    d.artifact = artifact.data();
    d.artifactBytes = artifact.size();
    g::PointInsertionHandle result;
    check(g::createPointInsertion(d, &result), "create insertion factory");
    return result;
}
void run(IDevice* device, unsigned components, unsigned attributeMask, unsigned count, bool root,
         bool translated, bool emptyTopology = false, bool support = false, bool poisonVelocity = false,
         float supportOffset = -.25f, bool cancelSentinel = false) {
    Harness h(device);
    const auto base = donut::app::GetDirectoryWithExecutable() / "shaders";
    const auto target = donut::app::GetShaderTypeName(device->getGraphicsAPI());
    const auto remap = artifact(base / "gvdb_point_cloud" / target / "GVDBAtlasRepack_relocateAtlas.bin");
    const auto conversion =
        artifact(base / "gvdb_point_cloud" / target / "GVDBPointStreamConversion_convertPointStream.bin");
    const auto shader = artifact(base / "gvdb_subcells" / target / "GVDBPointSubcells_expandSubcellPoints.bin");
    auto fixture=make(h,remap,2,emptyTopology?std::vector<g::Int3>{}:root?std::vector<g::Int3>{{-3,5,-2}}:std::vector<g::Int3>{{-8,0,0},{0,0,0},{8,0,0}},root?-1:1,root);
    g::PointOpsCreateDesc pc;
    pc.volume = fixture.owner;
    pc.conversionArtifact = conversion.data();
    pc.conversionArtifactBytes = conversion.size();
    g::PointOpsHandle pointFactory;
    check(g::createPointOps(pc, &pointFactory), "point factory");
    const auto origin = root ? g::Int3{-3, 5, -2} : g::Int3{-8, 0, 0};
    std::vector<g::Float4> positions(count), velocities(count);
    std::vector<uint32_t> colors(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t bits = i % 2 ? 0x80000000u : 0x7fc00123u;
        float extra;
        std::memcpy(&extra, &bits, 4);
        positions[i] = {float(origin.x) + float(i % 19) * .5f - 1.f, float(origin.y) + float(i % 7) * .5f,
                        float(origin.z) + float(i % 5) * .5f, extra};
        velocities[i] = {float(int(i % 9) - 4), -1, float(int(i % 7) - 3), extra};
        colors[i] = 0x80123400u + (i % 256);
        if (i % 23 == 0)
            positions[i].x = 10000;
        if (i > 1 && i % 13 == 0)
            positions[i] = positions[1];
        if (i % 29 == 0)
            positions[i].z = 1e10f;
    }
    if (poisonVelocity && count > 1)
        velocities[1].x = std::numeric_limits<float>::quiet_NaN();
    if (cancelSentinel && count)
        positions[0] = {float(origin.x), float(origin.y), 1e10f, 0};
    g::PointSetDesc uploadDesc;
    uploadDesc.positions.stream = {
        positions.data(), positions.size() * 16, 0, 16, count, g::PointComponent::Float32, components};
    if (attributeMask & 2)
        uploadDesc.velocities.stream = {
            velocities.data(), velocities.size() * 16, 0, 16, count, g::PointComponent::Float32, components};
    if (attributeMask & 4)
        uploadDesc.colors.stream = {colors.data(), colors.size() * 4, 0, 4, count, g::PointComponent::UInt8, 4};
    g::PointUploadTicketHandle upload;
    check(pointFactory->preparePoints(uploadDesc, {}, &upload), "prepare points");
    h.begin();
    check(pointFactory->recordPoints(h.context(), upload), "record points");
    h.submit();
    g::PointSetHandle source;
    check(upload->finishAfterCallerCompletion(&source), "finish points");
    upload = nullptr;
    pointFactory = nullptr;
    // Expansion promises exact canonical attribute transport. The preceding upload
    // applies affine conversion, which can change signed-zero/NaN representation.
    for (unsigned attribute = 0; attribute < 3; ++attribute)
        if (source->getDesc().attributeMask & (1u << attribute)) {
            g::BorrowedBytes canonical;
            check(source->getData(g::PointAttribute(attribute), canonical), "canonical source reference");
            for (uint32_t i = 0; i < count; ++i) {
                if (attribute == 0)
                    std::memcpy(&positions[i],
                                static_cast<const uint8_t*>(canonical.data) + size_t(i) * components * 4,
                                components * 4);
                else if (attribute == 1)
                    std::memcpy(&velocities[i],
                                static_cast<const uint8_t*>(canonical.data) + size_t(i) * components * 4,
                                components * 4);
                else
                    std::memcpy(&colors[i], static_cast<const uint8_t*>(canonical.data) + size_t(i) * 4, 4);
            }
        }
    g::PointInsertionRequestDesc request;
    request.snapshot = fixture.snapshot;
    request.points = source;
    const uint32_t subcellWidth = 4;
    request.mode = support ? g::PointInsertionMode::VelocitySupport : g::PointInsertionMode::Center;
    request.supportOffset = support ? supportOffset : 0.f;
    const uint32_t side = 4 / subcellWidth, perLeaf = side * side * side;

    if (translated)
        request.translation = {.5f, -.25f, .25f, 0};
    if (cancelSentinel)
        request.translation.z = -1e10f;
    struct Range {
        uint32_t offset, count;
    };
    if (poisonVelocity && support) {
        auto op = factory(device, shader);
        g::SubcellTicketHandle ticket;
        require(op->prepare(request, {}, &ticket) == FE_INVALID_ARGS && !ticket,
                "nonfinite direction rejected");
        return;
    }
    std::vector<Range> ranges(fixture.pool * perLeaf);
    std::vector<uint32_t> indices;
    for (uint32_t leaf = 0; leaf < fixture.pool; ++leaf)
        for (uint32_t local = 0; local < perLeaf; ++local) {
            auto& range = ranges[leaf * perLeaf + local];
            range.offset = uint32_t(indices.size());
            auto box = std::find_if(fixture.boxes.begin(), fixture.boxes.end(),
                                    [&](const auto& v) { return v.index == leaf; });
            if (box == fixture.boxes.end() || !box->active)
                continue;
            const int cell[] = {box->origin.x + int(local % side) * int(subcellWidth),
                                box->origin.y + int((local / side) % side) * int(subcellWidth),
                                box->origin.z + int(local / (side * side)) * int(subcellWidth)};
            for (uint32_t point = 0; point < count; ++point) {
                if (positions[point].z == 1e10f)
                    continue;
                float v[] = {positions[point].x + request.translation.x,
                             positions[point].y + request.translation.y,
                             positions[point].z + request.translation.z};
                if (support) {
                    v[0] += velocities[point].x * request.supportOffset;
                    v[1] += velocities[point].y * request.supportOffset;
                    v[2] += velocities[point].z * request.supportOffset;
                }
                bool contained = true;
                for (unsigned axis = 0; axis < 3; ++axis)
                    contained &= double(v[axis]) >= cell[axis] && double(v[axis]) < cell[axis] + 4;
                if (contained) {
                    indices.push_back(point);
                    ++range.count;
                }
            }
        }
    if (root && count) {
        auto rejectsArtifact = [&](const std::vector<char>& bytes) {
            g::SubcellCreateDesc d;
            d.device = device;
            d.artifact = bytes.data();
            d.artifactBytes = bytes.size();
            auto* out = reinterpret_cast<g::IGVDBPointInsertion*>(uintptr_t(1));
            require(NVRHI_FAILED(g::createPointInsertion(d, &out)) && !out,
                    "incompatible expansion artifact rejected");
        };
        rejectsArtifact({});
        rejectsArtifact({'b', 'a', 'd'});
        rejectsArtifact(rewriteArtifact(shader, [](auto& e, auto&) { e.ThreadGroupSize[0] = 128; }));
        rejectsArtifact(rewriteArtifact(shader, [](auto& e, auto&) { e.Name = e.Symbol = "wrongEntry"; }));
        rejectsArtifact(rewriteArtifact(shader, [](auto&, auto& bindings) {
            for (auto& v : bindings)
                if (v.Scope == ST_SCOPE_CONSTANT_BUFFER) {
                    v.Offset += 4;
                    break;
                }
        }));
        rejectsArtifact(rewriteArtifact(shader, [](auto&, auto& bindings) {
            for (auto& v : bindings)
                if (v.Kind == ST_BINDING_RW_BYTE_ADDRESS_BUFFER) {
                    v.RegisterIndex = 9;
                    break;
                }
        }));
    }
    auto operation = factory(device, shader), other = factory(device, shader);
    queryIdentity(operation.Get());
    g::SubcellsHandle supportInterface;
    check(operation->QueryInterface(uuid_of<g::IGVDBSubcells>(),
                                    reinterpret_cast<void**>(supportInterface.GetAddressOf())),
          "existing subcell interface");
    AutoPtr<IObject> insertionIdentity, subcellIdentity;
    check(operation->QueryInterface(uuid_of<IObject>(),
                                    reinterpret_cast<void**>(insertionIdentity.GetAddressOf())),
          "insertion identity");
    check(supportInterface->QueryInterface(uuid_of<IObject>(),
                                           reinterpret_cast<void**>(subcellIdentity.GetAddressOf())),
          "subcell identity");
    require(insertionIdentity == subcellIdentity, "canonical identity across interfaces");
    insertionIdentity = nullptr;
    subcellIdentity = nullptr;
    supportInterface = nullptr;
    const uint32_t mask = source->getDesc().attributeMask,
                   attributes = 1 + uint32_t((mask & 2) != 0) + uint32_t((mask & 4) != 0);
    const uint64_t stride = components * 4 + ((mask & 2) ? components * 4 : 0) + ((mask & 4) ? 4 : 0),
                   pairs = indices.size(), cells = ranges.size();
    const uint64_t host = 24 * cells + pairs * (4 + stride),
                   gpu = 24 * cells + (pairs ? 256 + 4 * pairs + 2 * pairs * stride + 4 * (3 - attributes) : 0);
    const uint32_t buffers = (cells ? 2 : 0) + (pairs ? 5 + attributes : 0);
    g::SubcellPreparationBudget budget;
    budget.maxCells = cells;
    budget.maxCandidateVisits = count;
    budget.maxPointPairs = pairs;
    budget.maxHostBytes = host;
    budget.maxAllocatedBytes = gpu;
    budget.maxBuffers = buffers;
    auto reject = [&](g::PointInsertionRequestDesc bad, g::SubcellPreparationBudget limit) {
        auto* result = reinterpret_cast<g::IGVDBSubcellTicket*>(uintptr_t(1));
        require(NVRHI_FAILED(operation->prepare(bad, limit, &result)) && !result,
                "bad preparation clears output");
    };
    auto bad = request;
    bad.snapshot = nullptr;
    reject(bad, {});
    bad = request;
    bad.points = nullptr;
    reject(bad, {});
    bad = request;
    bad.mode = g::PointInsertionMode(2);
    reject(bad, {});
    bad = request;
    bad.supportOffset = INFINITY;
    reject(bad, {});
    if (support && count > 19) {
        auto overflow = request;
        overflow.supportOffset = std::numeric_limits<float>::max();
        reject(overflow, {});
    }
    bad = request;
    bad.translation.x = INFINITY;
    reject(bad, {});
    bad = request;
    bad.reserved = 1;
    reject(bad, {});
    if (cells) {
        auto low = budget;
        --low.maxCells;
        reject(request, low);
        low = budget;
        --low.maxHostBytes;
        reject(request, low);
        low = budget;
        --low.maxAllocatedBytes;
        reject(request, low);
        low = budget;
        --low.maxBuffers;
        reject(request, low);
    }
    if (pairs) {
        auto low = budget;
        --low.maxPointPairs;
        reject(request, low);
        low = budget;
        low.maxCandidateVisits = 0;
        reject(request, low);
    }
    if (count) {
        auto low = budget;
        --low.maxCandidateVisits;
        reject(request, low);
    }
    if (!(mask & 2)) {
        auto missing = request;
        missing.mode = g::PointInsertionMode::VelocitySupport;
        reject(missing, {});
    }
    if (!support) {
        auto invalidOffset = request;
        invalidOffset.supportOffset = 1;
        reject(invalidOffset, {});
    }
    g::SubcellTicketHandle ticket;
    check(operation->prepare(request, budget, &ticket), "exact-budget subcells");
    queryIdentity(ticket.Get());
    const auto info = ticket->getDesc();
    require(info.cellCount == cells && info.pointPairs == pairs && info.leafPoolCount == fixture.pool &&
                info.subcellsPerLeaf == perLeaf && info.attributeMask == mask,
            "subcell metadata");
    g::BorrowedBytes cpuCells, cpuIndices;
    check(ticket->getCellRanges(cpuCells), "cell ranges");
    check(ticket->getSourceOrdinals(cpuIndices), "point ordinals");
    require(cpuCells.byteCount == cells * 8 &&
                (!cells || !std::memcmp(cpuCells.data, ranges.data(), size_t(cells * 8))),
            "brute force cell ranges");
    require(cpuIndices.byteCount == pairs * 4 &&
                (!pairs || !std::memcmp(cpuIndices.data, indices.data(), size_t(pairs * 4))),
            "stable brute force point order");
    std::vector<g::SubcellLocation> locations(size_t(cells), g::SubcellLocation{});
    for (const auto& box : fixture.boxes)
        if (box.active)
            for (uint32_t cell = 0; cell < perLeaf; ++cell)
                locations[box.index * perLeaf + cell] = {
                    {box.origin.x + int(cell % side) * int(subcellWidth),
                     box.origin.y + int((cell / side) % side) * int(subcellWidth),
                     box.origin.z + int(cell / (side * side)) * int(subcellWidth)},
                    box.index
                };
    g::BorrowedBytes cpuLocations;
    check(ticket->getCellLocations(cpuLocations), "cell locations");
    require(cpuLocations.byteCount == cells * 16 &&
                (!cells || !std::memcmp(cpuLocations.data, locations.data(), size_t(cells * 16))),
            "independent active flags, source leaves and signed origins");
    g::BorrowedBufferBinding early{nullptr, 13, 17};
    require(NVRHI_FAILED(ticket->getCellBuffer(early)) && early.byteOffset == 13 && early.byteSize == 17,
            "early buffer output preserved");
    g::PointSetHandle result;
    require(NVRHI_FAILED(ticket->finishAfterCallerCompletion(&result)) && !result, "early finish rejected");
    h.begin();
    require(NVRHI_FAILED(other->record(h.context(), ticket)), "wrong factory");
    require(NVRHI_FAILED(operation->record({}, ticket)), "wrong context");
    check(operation->record(h.context(), ticket), "record expansion");
    require(NVRHI_FAILED(operation->record(h.context(), ticket)), "one record");
    fixture.owner = nullptr;
    fixture.snapshot = nullptr;
    source = nullptr;
    operation = nullptr;
    other = nullptr;
    h.submit();
    check(ticket->finishAfterCallerCompletion(&result), "complete expanded GPU PointSet");
    require(result->getDesc().count == pairs && result->getDesc().attributeMask == mask,
            "expanded PointSet metadata");
    for (unsigned attribute = 0; attribute < 3; ++attribute)
        if (mask & (1u << attribute)) {
            g::BorrowedBytes bytes;
            g::BorrowedPointStream stream;
            check(result->getData(g::PointAttribute(attribute), bytes), "expanded CPU attribute");
            check(result->getStream(g::PointAttribute(attribute), stream), "expanded GPU attribute");
            require(bytes.byteCount == pairs * stream.byteStride, "expanded byte size");
            for (size_t i = 0; i < pairs; ++i) {
                const auto ordinal = indices[i];
                const auto* actual = static_cast<const uint8_t*>(bytes.data) + i * stream.byteStride;
                if (attribute == 0) {
                    auto expected = positions[ordinal];
                    expected.x += request.translation.x;
                    expected.y += request.translation.y;
                    expected.z += request.translation.z;
                    require(!std::memcmp(actual, &expected, components * 4),
                            "translated positions and exact W");
                } else if (attribute == 1)
                    require(!std::memcmp(actual, &velocities[ordinal], components * 4), "exact velocity order");
                else
                    require(!std::memcmp(actual, &colors[ordinal], 4), "exact color order");
            }
            const auto gpuBytes = h.read(stream.buffer, bytes.byteCount);
            require(gpuBytes.size() == bytes.byteCount &&
                        (!bytes.byteCount || !std::memcmp(gpuBytes.data(), bytes.data, gpuBytes.size())),
                    "GPU/CPU result coherence");
        }
    g::BorrowedBufferBinding mirror;
    check(ticket->getCellBuffer(mirror), "cell mirror");
    auto actual = h.read(mirror.buffer, mirror.byteSize);
    require(actual.size() == cells * 8 && (!cells || !std::memcmp(actual.data(), ranges.data(), actual.size())),
            "GPU cell ranges");
    check(ticket->getLocationBuffer(mirror), "location mirror");
    actual = h.read(mirror.buffer, mirror.byteSize);
    require(
        actual.size() == cells * 16 && (!cells || !std::memcmp(actual.data(), locations.data(), actual.size())),
        "GPU cell locations");
    check(ticket->getOrdinalBuffer(mirror), "ordinal mirror");
    actual = h.read(mirror.buffer, mirror.byteSize);
    require(
        actual.size() == pairs * 4 && (!pairs || !std::memcmp(actual.data(), indices.data(), actual.size())),
        "GPU point ordinals");
    g::PointSetHandle repeated;
    require(NVRHI_FAILED(ticket->finishAfterCallerCompletion(&repeated)) && !repeated, "one completion");
    g::Float4 lower{}, upper{};
    for (size_t i = 0; i < indices.size(); ++i) {
        auto v = positions[indices[i]];
        v.x += request.translation.x;
        v.y += request.translation.y;
        v.z += request.translation.z;
        v.w = 0;
        if (!i)
            lower = upper = v;
        else {
            lower = {std::min(lower.x, v.x), std::min(lower.y, v.y), std::min(lower.z, v.z), 0};
            upper = {std::max(upper.x, v.x), std::max(upper.y, v.y), std::max(upper.z, v.z), 0};
        }
    }
    const auto expandedInfo = result->getDesc();
    require(!std::memcmp(&lower, &expandedInfo.boundsMin, 16) &&
                !std::memcmp(&upper, &expandedInfo.boundsMax, 16) && expandedInfo.empty == uint32_t(!pairs),
            "independent expanded position bounds");
    ticket = nullptr;
    // The expanded set authenticates in both quantizers after source/ticket release.
    if (pairs && mask == 7) {
        const auto encode = artifact(base / "gvdb_quantize" / target / "GVDBPointQuantize_encodePointU16.bin");
        const auto decode = artifact(base / "gvdb_quantize" / target / "GVDBPointQuantize_decodePointU16.bin");
        g::PointQuantizeCreateDesc create;
        create.device = device;
        create.encodeArtifact = encode.data();
        create.encodeArtifactBytes = encode.size();
        create.decodeArtifact = decode.data();
        create.decodeArtifactBytes = decode.size();
        for (bool velocity : {false, true}) {
            g::PointQuantizeHandle quantizer;
            check((velocity ? g::createVelocityQuantize : g::createPointQuantize)(create, &quantizer),
                  "quantizer composition factory");
            g::BorrowedBytes canonical;
            g::BorrowedPointStream canonicalStream;
            const auto selected = velocity ? g::PointAttribute::Velocity : g::PointAttribute::Position;
            check(result->getData(selected, canonical), "prequantization expanded values");
            check(result->getStream(selected, canonicalStream), "prequantization layout");
            g::PointQuantizeRequestDesc q;
            q.points = result;
            q.bounds = velocity ? g::PointQuantizationBounds::AttributeExtrema
                                : g::PointQuantizationBounds::LegacyPadded;
            g::PointQuantizeTicketHandle qt;
            check(quantizer->prepare(q, {}, &qt), "quantizer composition prepare");
            h.begin();
            check(quantizer->record(h.context(), qt), "quantizer composition record");
            h.submit();
            g::PointSetHandle decoded;
            check(qt->finishAfterCallerCompletion(&decoded), "quantizer composition complete");
            g::BorrowedBytes codes;
            check(qt->getCodes(codes), "expanded packed ushort3");
            require(codes.byteCount == pairs * 6, "packed pair count");
            g::Float4 minimum, range;
            qt->getBounds(minimum, range);
            const float minima[] = {minimum.x, minimum.y, minimum.z}, spans[] = {range.x, range.y, range.z};
            for (size_t i = 0; i < pairs; ++i)
                for (unsigned axis = 0; axis < 3; ++axis) {
                    float value;
                    uint16_t code;
                    std::memcpy(
                        &value,
                        static_cast<const uint8_t*>(canonical.data) + i * canonicalStream.byteStride + axis * 4,
                        4);
                    std::memcpy(&code, static_cast<const uint8_t*>(codes.data) + (i * 3 + axis) * 2, 2);
                    const double normalized = std::clamp((double(value) - minima[axis]) / spans[axis], 0., 1.);
                    require(std::abs(int(code) - int(normalized * 65535.)) <= 1,
                            "independent expanded attribute code and association");
                }
            result = decoded;
        }
    }
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
                for (unsigned mask : {1u, 3u, 5u, 7u}) {
                    run(device.device, components, mask, 257, false, true);
                    if (mask & 2)
                        run(device.device, components, mask, 257, false, true, false, true);
                }
            run(device.device, 4, 7, 19, true, false);
            run(device.device, 4, 7, 19, true, true, false, true);
            run(device.device, 3, 3, 19, false, false, false, true);
            run(device.device, 4, 7, 1, false, false);
            run(device.device, 3, 1, 0, false, false);
            run(device.device, 4, 7, 5, false, true, true);
            run(device.device, 4, 7, 5, false, true, true, true);
            run(device.device, 3, 3, 19, true, false, false, true, false, .75f);
            run(device.device, 3, 3, 19, true, false, false, true, true);
            run(device.device, 4, 3, 19, true, false, false, false, true);
            run(device.device, 4, 7, 1, false, false, false, false, false, 0, true);
        }
        require(problems == 0, "native validation warnings/errors");
        std::puts(
            "PASS retained insertion: brute-force ranges, stable attributes, budgets, lifetime and packed "
            "composition");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}

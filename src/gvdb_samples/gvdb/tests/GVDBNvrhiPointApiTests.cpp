// SPDX-License-Identifier: Apache-2.0
// Public-interface fixture: never includes concrete HostVolume/NvrhiVolume headers.
#include "GVDBNvrhiPointApi.h"
#include "GVDBNvrhiForeignVolume.h"
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
#include <limits>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <set>
#include <cmath>
#include <cfloat>
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
        for (int i = 1; i < argc; ++i) {
            useCuda |= !std::strcmp(argv[i], "-cuda");
            validation |= !std::strcmp(argv[i], "--validation");
        }
        if (useCuda) {
#if ETHEREAL_TEST_CUDA
            cuda::DeviceDesc desc;
            desc.enableOptiX = false;
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
            desc.enableNvrhiValidationLayer = true;
            desc.enableDebugRuntime = validation;
            require(manager->CreateHeadlessDevice(desc), "create graphics device");
            device = manager->GetDevice();
        }
        require(bool(device), "create test device");
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
g::VolumeHandle makeVolume(IDevice* device, const std::vector<char>& remap) {
    g::VolumeCreateDesc desc;
    desc.device = device;
    desc.configuration.levelCount = 5;
    desc.configuration.logDimension = {2, 1, 1, 1, 1};
    desc.remapArtifact = remap.data();
    desc.remapArtifactBytes = remap.size();
    g::VolumeHandle result;
    check(g::createVolume(desc, &result), "create point fixture volume");
    return result;
}
g::PointOpsHandle makeOps(g::IGVDBVolume* owner, const std::vector<char>& artifact) {
    g::PointOpsCreateDesc desc;
    desc.volume = owner;
    desc.conversionArtifact = artifact.data();
    desc.conversionArtifactBytes = artifact.size();
    g::PointOpsHandle result;
    check(g::createPointOps(desc, &result), "create point factory");
    return result;
}
g::SnapshotHandle snapshot(g::IGVDBVolume* volume) {
    g::SnapshotHandle result;
    check(volume->getSnapshot(&result), "retain volume generation");
    return result;
}
void put(std::vector<uint8_t>& bytes, size_t offset, g::PointComponent component, float value) {
    if (component == g::PointComponent::UInt8) {
        const auto v = uint8_t(value);
        std::memcpy(bytes.data() + offset, &v, 1);
    } else if (component == g::PointComponent::UInt16) {
        const auto v = uint16_t(value);
        std::memcpy(bytes.data() + offset, &v, 2);
    } else if (component == g::PointComponent::Float32)
        std::memcpy(bytes.data() + offset, &value, 4);
    else {
        const double v = value;
        std::memcpy(bytes.data() + offset, &v, 8);
    }
}
uint32_t componentWidth(g::PointComponent component) {
    switch (component) {
        case g::PointComponent::UInt8:
            return 1;
        case g::PointComponent::UInt16:
            return 2;
        case g::PointComponent::Float32:
            return 4;
        case g::PointComponent::Float64:
            return 8;
    }
    throw std::runtime_error("fixture encoding");
}
void verifyData(Harness& h, g::IGVDBPointSet* points, g::PointAttribute attribute,
                const std::vector<uint8_t>& expected, uint32_t components, g::PointComponent component) {
    g::BorrowedBytes bytes;
    check(points->getData(attribute, bytes), "get immutable converted CPU bytes");
    require(bytes.byteCount == expected.size() &&
                (!bytes.byteCount || !std::memcmp(bytes.data, expected.data(), expected.size())),
            "converted CPU bytes/association differ from independent arithmetic");
    g::BorrowedPointStream stream;
    check(points->getStream(attribute, stream), "get canonical GPU stream");
    require(stream.components == components && stream.component == component && stream.byteOffset == 0 &&
                stream.byteStride == components * componentWidth(component) &&
                stream.byteSize == expected.size() && stream.count == points->getDesc().count,
            "canonical point stream descriptor");
    require(h.read(stream.buffer, stream.byteSize) == expected,
            "canonical GPU bytes differ from retained CPU bytes");
}
g::PointSetHandle finish(Harness& h, g::IGVDBPointOps* ops, const g::PointSetDesc& desc) {
    g::PointUploadTicketHandle ticket;
    check(ops->preparePoints(desc, {}, &ticket), "prepare copied host points");
    h.begin();
    check(ops->recordPoints(h.context(), ticket), "record GPU point conversion");
    h.submit();
    g::PointSetHandle result;
    check(ticket->finishAfterCallerCompletion(&result), "finish actual converted points");
    return result;
}
g::PointSetHandle points(Harness& h, g::IGVDBPointOps* ops, const std::vector<g::Float4>& values) {
    g::PointSetDesc desc;
    desc.positions.stream = {values.data(), values.size() * sizeof(g::Float4), 0, sizeof(g::Float4),
                             values.size(), g::PointComponent::Float32,        4};
    return finish(h, ops, desc);
}
class ForeignSet final : public ObjectImpl<g::IGVDBPointSet> {
    IDevice* m_device;

 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ForeignSet)
    NVRHI_IMPLEMENTS_INTERFACE(g::IGVDBPointSet)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    explicit ForeignSet(IDevice* device)
        : m_device(device) {}
    IDevice* getDevice() const noexcept override { return m_device; }
    g::PointSetInfo getDesc() const noexcept override { return {}; }
    FRESULT getStream(g::PointAttribute, g::BorrowedPointStream& out) const noexcept override {
        out = {};
        return FE_UNSUPPORTED;
    }
    FRESULT getData(g::PointAttribute, g::BorrowedBytes& out) const noexcept override {
        out = {};
        return FE_UNSUPPORTED;
    }
};
class ForeignUpload final : public ObjectImpl<g::IGVDBPointUploadTicket> {
    IDevice* m_device;

 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ForeignUpload)
    NVRHI_IMPLEMENTS_INTERFACE(g::IGVDBPointUploadTicket)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    explicit ForeignUpload(IDevice* device)
        : m_device(device) {}
    IDevice* getDevice() const noexcept override { return m_device; }
    FRESULT finishAfterCallerCompletion(g::IGVDBPointSet** out) noexcept override {
        if (out)
            *out = nullptr;
        return FE_UNSUPPORTED;
    }
};
void conversions(Harness& h, g::IGVDBPointOps* ops) {
    for (auto component : {g::PointComponent::UInt8, g::PointComponent::UInt16, g::PointComponent::Float32,
                           g::PointComponent::Float64}) {
        const uint32_t width = componentWidth(component), count = 257;
        const uint64_t stride = width * 12 + 13;
        const uint64_t offsets[] = {1, 4 * width + 4, 8 * width + 7};
        std::vector<uint8_t> source(size_t(count * stride + 5), 0xe7);
        std::array<std::vector<uint8_t>, 3> expected;
        expected[0].resize(count * 16);
        expected[1].resize(count * 16);
        expected[2].resize(count * 4);
        g::Float4 minimum{FLT_MAX, FLT_MAX, FLT_MAX, 0}, maximum{-FLT_MAX, -FLT_MAX, -FLT_MAX, 0};
        for (uint32_t p = 0; p < count; ++p) {
            float position[4]{}, velocity[4]{};
            for (uint32_t k = 0; k < 4; ++k) {
                const float a = float(2 + (p + 3 * k) % 17), b = float(5 + (2 * p + k) % 23),
                            c = float((p * 3 + k * 71) % 251);
                put(source, size_t(p * stride + offsets[0] + k * width), component, a);
                put(source, size_t(p * stride + offsets[1] + k * width), component, b);
                put(source, size_t(p * stride + offsets[2] + k * width), component, c);
                // Exact binary fractions avoid imposing a CPU FMA/denormal policy.
                position[k] = a * .5f - 3.f;
                velocity[k] = b * 2.f - 25.f;
                expected[2][p * 4 + k] = uint8_t(std::max(0.f, std::min(255.f, c * 2.f - 15.f)));
            }
            std::memcpy(expected[0].data() + p * 16, position, 16);
            std::memcpy(expected[1].data() + p * 16, velocity, 16);
            minimum.x = std::min(minimum.x, position[0]);
            minimum.y = std::min(minimum.y, position[1]);
            minimum.z = std::min(minimum.z, position[2]);
            maximum.x = std::max(maximum.x, position[0]);
            maximum.y = std::max(maximum.y, position[1]);
            maximum.z = std::max(maximum.z, position[2]);
        }
        g::PointSetDesc desc;
        auto fill = [&](g::HostPointAttributeDesc& attribute, uint32_t index, float scale, float translation) {
            attribute.stream = {source.data(), source.size(), offsets[index], stride, count, component, 4};
            attribute.transform.scale = {scale, scale, scale, scale};
            attribute.transform.translation = {translation, translation, translation, translation};
        };
        fill(desc.positions, 0, .5f, -3);
        fill(desc.velocities, 1, 2, -25);
        fill(desc.colors, 2, 2, -15);
        g::PointUploadTicketHandle ticket;
        check(ops->preparePoints(desc, {}, &ticket), "prepare odd interleaved point attributes");
        queryIdentity(ticket.Get());
        auto* rejected = reinterpret_cast<g::IGVDBPointSet*>(uintptr_t(1));
        require(NVRHI_FAILED(ticket->finishAfterCallerCompletion(&rejected)) && !rejected,
                "finish before record must clear output");
        std::fill(source.begin(), source.end(), 0x19);  // Preparation must have copied every strided stream.
        h.begin();
        check(ops->recordPoints(h.context(), ticket), "record odd input conversion");
        require(NVRHI_FAILED(ops->recordPoints(h.context(), ticket)), "upload ticket recorded twice");
        h.submit();
        g::PointSetHandle set;
        check(ticket->finishAfterCallerCompletion(&set), "finish converted attributes");
        queryIdentity(set.Get());
        const auto info = set->getDesc();
        require(info.count == count && info.attributeMask == 7 && !info.empty &&
                    !std::memcmp(&info.boundsMin, &minimum, sizeof(minimum)) &&
                    !std::memcmp(&info.boundsMax, &maximum, sizeof(maximum)),
                "actual converted point bounds or attribute count");
        for (uint32_t a = 0; a < 3; ++a)
            verifyData(h, set, g::PointAttribute(a), expected[a], 4,
                       a == 2 ? g::PointComponent::UInt8 : g::PointComponent::Float32);
        rejected = reinterpret_cast<g::IGVDBPointSet*>(uintptr_t(1));
        require(NVRHI_FAILED(ticket->finishAfterCallerCompletion(&rejected)) && !rejected,
                "successful finish consumes upload ticket");
        g::BorrowedBytes invalid{reinterpret_cast<void*>(uintptr_t(1)), 99};
        require(set->getData(g::PointAttribute(99), invalid) == FE_INVALID_ARGS && !invalid.data &&
                    !invalid.byteCount,
                "invalid attribute clears bytes");
    }
    std::puts(
        "PASS public point conversion: four encodings, odd strided attributes, association, bounds and ticket "
        "states");
}
void invalidInputs(Harness& h, g::IGVDBPointOps* ops, const std::vector<char>& artifact) {
    const float scalar[3] = {1, 2, 3};
    g::PointSetDesc valid;
    valid.positions.stream = {scalar, sizeof(scalar), 0, sizeof(scalar), 1, g::PointComponent::Float32, 3};
    auto rejects = [&](const g::PointSetDesc& desc, const g::PointPreparationBudget& budget = {}) {
        auto* out = reinterpret_cast<g::IGVDBPointUploadTicket*>(uintptr_t(1));
        require(NVRHI_FAILED(ops->preparePoints(desc, budget, &out)) && !out,
                "invalid point descriptor/budget not rejected atomically");
    };
    auto bad = valid;
    bad.structSize--;
    rejects(bad);
    bad = valid;
    bad.reserved = 1;
    rejects(bad);
    bad = valid;
    bad.positions.stream.byteSize = 11;
    rejects(bad);
    bad = valid;
    bad.positions.stream.byteOffset = UINT64_MAX;
    rejects(bad);
    bad = valid;
    bad.positions.stream.byteStride = 11;
    rejects(bad);
    bad = valid;
    bad.positions.stream.count = UINT64_MAX;
    rejects(bad);
    bad = valid;
    bad.positions.stream.components = 2;
    rejects(bad);
    bad = valid;
    bad.positions.stream.component = g::PointComponent(99);
    rejects(bad);
    bad = valid;
    bad.positions.stream.data = nullptr;
    rejects(bad);
    bad = valid;
    bad.positions.transform.scale.x = std::numeric_limits<float>::infinity();
    rejects(bad);
    bad = valid;
    bad.velocities = valid.positions;
    bad.velocities.stream.count = 2;
    rejects(bad);
    bad = valid;
    bad.colors = valid.positions;
    rejects(bad);
    g::PointPreparationBudget budget;
    budget.reserved = 1;
    rejects(valid, budget);
    budget = {};
    budget.maxInputBytes = 11;
    rejects(valid, budget);
    budget = {};
    budget.maxAllocatedBytes = 1;
    rejects(valid, budget);
    budget = {};
    budget.maxBuffers = 3;
    rejects(valid, budget);
    require(ops->preparePoints(valid, {}, nullptr) == FE_INVALID_ARGS, "null preparePoints output");
    g::PointUploadTicketHandle ticket;
    check(ops->preparePoints(valid, {}, &ticket), "prepare foreign-ticket checks");
    auto secondFactory = makeOps(ops->getVolume(), artifact);
    auto foreignTicket = MAKE_RC_OBJ_PTR(ForeignUpload, h.device);
    h.begin();
    require(NVRHI_FAILED(secondFactory->recordPoints(h.context(), ticket)),
            "another factory recorded opaque upload ticket");
    require(NVRHI_FAILED(ops->recordPoints(h.context(), foreignTicket)),
            "foreign upload implementation accepted");
    require(NVRHI_FAILED(ops->recordPoints({nullptr, h.commands}, ticket)),
            "invalid recording device accepted");
    require(NVRHI_FAILED(ops->recordPoints({h.device, nullptr}, ticket)), "null recording commands accepted");
    check(ops->recordPoints(h.context(), ticket), "valid ticket usable after rejected recording");
    h.submit();
    g::PointSetHandle set;
    check(ticket->finishAfterCallerCompletion(&set), "finish retry ticket");
    g::BorrowedBytes absent{reinterpret_cast<void*>(uintptr_t(1)), 99};
    require(
        set->getData(g::PointAttribute::Velocity, absent) == FE_NOT_FOUND && !absent.data && !absent.byteCount,
        "missing velocity semantics");
    // Finite XYZ is validated only after the actual GPU conversion. Nonfinite W
    // and velocity are permitted, and must not accidentally affect geometric bounds.
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    const std::vector<g::Float4> nonfiniteW{
        {1, 2, 3, nan},
        {4, 5, 6, inf}
    };
    g::PointSetDesc allowedDesc;
    allowedDesc.positions.stream = {nonfiniteW.data(), nonfiniteW.size() * 16,     0, 16,
                                    nonfiniteW.size(), g::PointComponent::Float32, 4};
    const std::vector<g::Float4> nonfiniteVelocity{
        {nan, inf, -inf, -0.f},
        {1,   2,   3,    4   }
    };
    allowedDesc.velocities.stream = {nonfiniteVelocity.data(), nonfiniteVelocity.size() * 16, 0, 16,
                                     nonfiniteVelocity.size(), g::PointComponent::Float32,    4};
    auto allowed = finish(h, ops, allowedDesc);
    std::vector<uint8_t> velocityBytes(nonfiniteVelocity.size() * 16);
    std::memcpy(velocityBytes.data(), nonfiniteVelocity.data(), velocityBytes.size());
    verifyData(h, allowed, g::PointAttribute::Velocity, velocityBytes, 4, g::PointComponent::Float32);
    require(allowed->getDesc().boundsMax.z == 6, "W contaminated geometric validation");
    for (float value : {nan, inf, -inf}) {
        const std::vector<g::Float4> invalid{
            {value, 2, 3, 0}
        };
        g::PointSetDesc desc;
        desc.positions.stream = {
            invalid.data(), sizeof(g::Float4), 0, sizeof(g::Float4), 1, g::PointComponent::Float32, 4};
        check(ops->preparePoints(desc, {}, &ticket), "defer nonfinite input validation until conversion");
        h.begin();
        check(ops->recordPoints(h.context(), ticket), "record nonfinite input");
        h.submit();
        auto* output = reinterpret_cast<g::IGVDBPointSet*>(uintptr_t(1));
        require(NVRHI_FAILED(ticket->finishAfterCallerCompletion(&output)) && !output,
                "nonfinite XYZ produced a usable point set");
    }
    g::PointSetDesc empty;
    budget = {};
    budget.maxInputBytes = 0;
    budget.maxAllocatedBytes = 0;
    budget.maxBuffers = 0;
    check(ops->preparePoints(empty, budget, &ticket), "zero points require no GPU buffers");
    h.begin();
    check(ops->recordPoints(h.context(), ticket), "record empty upload");
    h.submit();
    check(ticket->finishAfterCallerCompletion(&set), "finish empty immutable point set");
    require(set->getDesc().count == 0 && set->getDesc().empty && set->getDesc().attributeMask == 1,
            "empty PointSet shape");
    g::BorrowedPointStream stream;
    check(set->getStream(g::PointAttribute::Position, stream), "empty position stream");
    require(!stream.buffer && !stream.byteSize && !stream.count, "empty PointSet fabricated GPU data");
    std::puts(
        "PASS public point descriptor failures, empty datasets, nonfinite XYZ and foreign ticket "
        "authentication");
}
using Coordinate = std::array<int32_t, 3>;
using LeafSet = std::set<Coordinate>;
LeafSet leaves(g::IGVDBVolume* owner) {
    LeafSet found;
    const auto pool = owner->getPool(0, 0);
    const auto* bytes = static_cast<const uint8_t*>(pool.data);
    for (uint64_t i = 0; i < pool.count; ++i) {
        g::Node node;
        std::memcpy(&node, bytes + i * pool.stride, sizeof(node));
        if (node.flags)
            require(found.insert({node.position.x, node.position.y, node.position.z}).second,
                    "duplicate active leaf");
    }
    require(found.size() == owner->getActiveLeafCount(),
            "host active count differs from inspected node records");
    return found;
}
LeafSet cube(std::initializer_list<int32_t> coordinates) {
    LeafSet result;
    for (auto x : coordinates)
        for (auto y : coordinates)
            for (auto z : coordinates)
                result.insert({x, y, z});
    return result;
}
void publish(Harness& h, g::IGVDBVolume* owner, g::IGVDBGenerationTicket* ticket) {
    h.begin();
    check(owner->recordAndPublish(h.context(), ticket), "record public point topology publication");
    h.submit();
}
g::GenerationTicketHandle prepareTopology(g::IGVDBPointOps* ops, g::IGVDBPointSet* set,
                                          g::PointTopologyMode mode, float radius = 0, uint32_t minDepth = 0) {
    g::PointTopologyDesc desc;
    desc.mode = mode;
    desc.radius = radius;
    desc.minDepth = minDepth;
    g::PlacementDesc placement;
    placement.minimumBrickCount = {1, 1, 1};
    g::GenerationTicketHandle ticket;
    check(ops->prepareTopology(set, desc, placement, {}, &ticket), "prepare public point topology");
    return ticket;
}
void topologyGoldens(Harness& h, const std::vector<char>& remap, const std::vector<char>& artifact) {
    auto owner = makeVolume(h.device, remap);
    auto ops = makeOps(owner, artifact);
    queryIdentity(ops.Get());
    auto a = points(h, ops,
                    {
                        {3.75f, 3.75f, 3.75f, 0}
    });
    auto ticket = prepareTopology(ops, a, g::PointTopologyMode::FullRebuild, .5f);
    const auto initialEpoch = owner->getTopologyRevision().epoch;
    publish(h, owner, ticket);
    require(leaves(owner) == cube({0}) && owner->getTopologyRevision().epoch > initialEpoch,
            "full rebuild radius<1 must use center only");
    a = points(h, ops,
               {
                   {6.75f, 6.75f, 6.75f, 0}
    });
    ticket = prepareTopology(ops, a, g::PointTopologyMode::FullRebuild, 1.25f);
    publish(h, owner, ticket);
    require(leaves(owner) == cube({4, 8}), "full support positive upper bound is int(int(p)+2r), inclusive");
    auto appendOwner = makeVolume(h.device, remap);
    auto appendOps = makeOps(appendOwner, artifact);
    auto appendEpoch = appendOwner->getTopologyRevision().epoch;
    ticket = prepareTopology(appendOps, a, g::PointTopologyMode::Accumulate, 1.25f);
    publish(h, appendOwner, ticket);
    require(leaves(appendOwner) == cube({4}) && appendOwner->getTopologyRevision().epoch == appendEpoch,
            "accumulation support positive upper bound is int(int(p)+r), not full-rebuild +2r");
    a = points(h, ops,
               {
                   {4.25f, 4.25f, 4.25f, 0}
    });
    auto narrowOwner = makeVolume(h.device, remap);
    auto narrowOps = makeOps(narrowOwner, artifact);
    ticket = prepareTopology(narrowOps, a, g::PointTopologyMode::Accumulate, .75f);
    publish(h, narrowOwner, ticket);
    require(leaves(narrowOwner) == cube({0, 4}), "accumulation still expands support for radius<1");
    auto truncOwner = makeVolume(h.device, remap);
    auto truncOps = makeOps(truncOwner, artifact);
    a = points(h, ops,
               {
                   {3.75f, 3.75f, 3.75f, 0}
    });
    ticket = prepareTopology(truncOps, a, g::PointTopologyMode::Accumulate, .5f);
    publish(h, truncOwner, ticket);
    require(leaves(truncOwner) == cube({0}),
            "support casts position before radius; untruncated p+r would add cells");
    a = points(h, ops,
               {
                   {-.25f, .25f, .25f, 0}
    });
    ticket = prepareTopology(ops, a, g::PointTopologyMode::FullRebuild, 0, 3);
    publish(h, owner, ticket);
    require(leaves(owner) ==
                    LeafSet{
                        {-4, 0, 0}
    } &&
                owner->getRootId().level() >= 3,
            "signed center retained and minimum root level enforced");
    auto signedOwner = makeVolume(h.device, remap);
    auto signedOps = makeOps(signedOwner, artifact);
    ticket = prepareTopology(signedOps, a, g::PointTopologyMode::Accumulate);
    publish(h, signedOwner, ticket);
    require(leaves(signedOwner) ==
                LeafSet{
                    {-4, 0, 0},
                    {0,  0, 0}
    },
            "signed accumulation unions the containing brick with truncated legacy support");
    auto duplicates = points(h, ops,
                             {
                                 {6.75f, 6.75f, 6.75f, 0},
                                 {6.75f, 6.75f, 6.75f, 0}
    });
    g::PointTopologyDesc counted;
    counted.radius = 1.25f;
    g::PlacementDesc countedPlacement;
    countedPlacement.minimumBrickCount = {1, 1, 1};
    g::PointPreparationBudget countedBudget;
    countedBudget.maxActivationCandidates = 17;
    auto* countRejected = reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
    require(NVRHI_FAILED(signedOps->prepareTopology(duplicates, counted, countedPlacement, countedBudget,
                                                    &countRejected)) &&
                !countRejected,
            "activation budget must count both center visits and duplicate support visits");
    countedBudget.maxActivationCandidates = 18;
    check(signedOps->prepareTopology(duplicates, counted, countedPlacement, countedBudget, &ticket),
          "exact activation budget boundary");
    publish(h, signedOwner, ticket);
    require(leaves(signedOwner) == cube({4, 8}),
            "duplicate points must deduplicate only topology, not visit budget");
    auto epoch = owner->getTopologyRevision().epoch;
    const auto survivor = owner->findLeaf({-4, 0, 0});
    // Existing inactive leaf becomes active even though the request adds no nodes.
    g::NodeId dormant;
    check(owner->activate(0, 0, 0, dormant), "allocate dormant neighbor");
    check(owner->setActive(dormant, false), "make existing leaf inactive");
    check(owner->finishTopology(), "finish dormant neighbor");
    g::GenerationDesc generation;
    generation.placement.minimumBrickCount = {1, 1, 1};
    check(owner->prepareGeneration(generation, &ticket), "publish inactive allocated neighbor");
    publish(h, owner, ticket);
    const auto leafCount = owner->getPoolCount(0, 0);
    auto reactivation = points(h, ops,
                               {
                                   {.25f, .25f, .25f, 0}
    });
    ticket = prepareTopology(ops, reactivation, g::PointTopologyMode::Accumulate);
    publish(h, owner, ticket);
    require(owner->getPoolCount(0, 0) == leafCount && owner->findLeaf({0, 0, 0}) == dormant &&
                owner->isActive({0, 0, 0}) && owner->findLeaf({-4, 0, 0}) == survivor &&
                owner->getTopologyRevision().epoch == epoch,
            "existing-only reactivation lost IDs or needed a newly allocated leaf");
    auto empty = points(h, ops, {});
    ticket = prepareTopology(ops, empty, g::PointTopologyMode::Accumulate);
    publish(h, owner, ticket);
    require(leaves(owner) ==
                    LeafSet{
                        {-4, 0, 0},
                        {0,  0, 0}
    } &&
                owner->getTopologyRevision().epoch == epoch,
            "empty accumulation must keep existing leaves");
    ticket = prepareTopology(ops, a, g::PointTopologyMode::FullRebuild);
    publish(h, owner, ticket);
    require(leaves(owner) ==
                    LeafSet{
                        {-4, 0, 0}
    } &&
                owner->getTopologyRevision().epoch > epoch,
            "subset-only full rebuild must remove old leaves");
    epoch = owner->getTopologyRevision().epoch;
    ticket = prepareTopology(ops, empty, g::PointTopologyMode::FullRebuild);
    publish(h, owner, ticket);
    require(leaves(owner).empty() && owner->getRootId().isNull() && owner->getTopologyRevision().epoch > epoch,
            "empty full rebuild must clear topology");
    // Invalid or over-budget requests must leave live CPU/GPU metadata unchanged.
    const auto before = snapshot(owner);
    const auto revision = owner->getTopologyRevision();
    auto rejects = [&](g::IGVDBPointSet* set, const g::PointTopologyDesc& desc,
                       const g::PlacementDesc& placement, const g::PointPreparationBudget& budget = {}) {
        auto* output = reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
        require(NVRHI_FAILED(ops->prepareTopology(set, desc, placement, budget, &output)) && !output,
                "invalid topology request not rejected");
        const auto after = snapshot(owner)->getDesc();
        require(owner->getTopologyRevision().epoch == revision.epoch &&
                    owner->getTopologyRevision().topology == revision.topology &&
                    after.revision.placement == before->getDesc().revision.placement && leaves(owner).empty(),
                "rejected topology changed owner");
    };
    g::PointTopologyDesc desc;
    g::PlacementDesc placement;
    placement.minimumBrickCount = {1, 1, 1};
    auto foreign = MAKE_RC_OBJ_PTR(ForeignSet, h.device);
    rejects(foreign, desc, placement);
    rejects(nullptr, desc, placement);
    desc.radius = -1;
    rejects(a, desc, placement);
    desc.radius = std::numeric_limits<float>::quiet_NaN();
    rejects(a, desc, placement);
    desc = {};
    desc.minDepth = 5;
    rejects(a, desc, placement);
    desc = {};
    desc.mode = g::PointTopologyMode(99);
    rejects(a, desc, placement);
    desc = {};
    desc.structSize--;
    rejects(a, desc, placement);
    desc = {};
    desc.reserved = 1;
    rejects(a, desc, placement);
    desc = {};
    desc.preserveMetadata = 2;
    rejects(a, desc, placement);
    desc = {};
    desc.reserved2 = 1;
    rejects(a, desc, placement);
    desc = {};
    desc.mode = g::PointTopologyMode::Accumulate;
    desc.preserveMetadata = 0;
    rejects(a, desc, placement);
    desc.preserveMetadata = 1;
    placement.preserve = 0;
    rejects(a, desc, placement);
    placement.preserve = 1;
    desc = {};
    g::PointPreparationBudget budget;
    budget.maxActivationCandidates = 0;
    rejects(a, desc, placement, budget);
    budget = {};
    budget.maxBuffers = 0;
    rejects(a, desc, placement, budget);
    const auto oversized = points(h, ops,
                                  {
                                      {16777216.f, 0, 0, 0}
    });
    rejects(oversized, desc, placement);
    // Preparation captures a finished immutable set, but not permission to publish
    // after a later host edit or through another volume.
    ticket = prepareTopology(ops, a, g::PointTopologyMode::FullRebuild);
    h.begin();
    require(NVRHI_FAILED(appendOwner->recordAndPublish(h.context(), ticket)),
            "foreign volume published topology ticket");
    h.submit();
    check(owner->finishTopology(), "invalidate prepared host revision");
    h.begin();
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(), ticket)), "stale point topology published");
    h.submit();
    std::puts(
        "PASS public point topology: independent full/append support, signed coordinates, stable IDs, "
        "empty/subset removal and failures");
}

void precisionAndColors(Harness& h, g::IGVDBPointOps* ops) {
    // Binary64 ties are specified independently as expected binary32 bit patterns.
    const double source[][3] = {
        {1.0 + 0x1p-24,     2, 3},
        {1.0 + 3 * 0x1p-24, 4, 5},
        {-0.0,              6, 7},
        {0x1p-149,          8, 9}
    };
    const uint32_t expectedX[] = {0x3f800000u, 0x3f800002u, 0x80000000u, 1u};
    const float velocities[][3] = {
        {-0.f, 1,  2 },
        {3,    4,  5 },
        {6,    7,  8 },
        {9,    10, 11}
    };
    const float colors[][4] = {
        {-.25f, 0,    .5f,  1},
        {2,     .25f, .75f, 1},
        {0,     0,    0,    0},
        {1,     1,    1,    1}
    };
    g::PointSetDesc desc;
    desc.positions.stream = {source, sizeof(source), 0, sizeof(source[0]), 4, g::PointComponent::Float64, 3};
    desc.velocities.stream = {velocities, sizeof(velocities),         0, sizeof(velocities[0]),
                              4,          g::PointComponent::Float32, 3};
    desc.colors.stream = {colors, sizeof(colors), 0, sizeof(colors[0]), 4, g::PointComponent::Float32, 4};
    desc.colors.transform.scale = {255, 255, 255, 255};
    auto set = finish(h, ops, desc);
    std::vector<uint8_t> position(4 * 12), velocity(sizeof(velocities));
    std::memcpy(velocity.data(), velocities, sizeof(velocities));
    for (uint32_t i = 0; i < 4; ++i) {
        std::memcpy(position.data() + i * 12, &expectedX[i], 4);
        const float yz[] = {float(source[i][1]), float(source[i][2])};
        std::memcpy(position.data() + i * 12 + 4, yz, 8);
    }
    const std::vector<uint8_t> color{0, 0, 127, 255, 255, 63, 191, 255, 0, 0, 0, 0, 255, 255, 255, 255};
    verifyData(h, set, g::PointAttribute::Position, position, 3, g::PointComponent::Float32);
    verifyData(h, set, g::PointAttribute::Velocity, velocity, 3, g::PointComponent::Float32);
    verifyData(h, set, g::PointAttribute::Color, color, 4, g::PointComponent::UInt8);
    std::puts("PASS public binary64 ties/subnormal/signed-zero and explicit normalized-float color conversion");
}
void retainedChannels(Harness& h, const std::vector<char>& remap, const std::vector<char>& artifact) {
    auto owner = makeVolume(h.device, remap);
    auto ops = makeOps(owner, artifact);
    check(owner->setChannel(17,
                            {
                                g::AtlasFormat::R32_FLOAT, {-9, 0, 0, 0}
    }),
          "configure selected nonzero channel");
    auto originalPoints = points(h, ops,
                                 {
                                     {.25f, .25f, .25f, 0}
    });
    auto ticket = prepareTopology(ops, originalPoints, g::PointTopologyMode::FullRebuild);
    publish(h, owner, ticket);
    std::vector<float> fullAtlas(6 * 6 * 6, 77.f);
    g::ChannelUploadDesc upload{17, 0, fullAtlas.data(), fullAtlas.size() * 4};
    g::GenerationDesc generation;
    generation.placement.minimumBrickCount = {1, 1, 1};
    generation.uploads = &upload;
    generation.uploadCount = 1;
    check(owner->prepareGeneration(generation, &ticket), "seed old field interior");
    publish(h, owner, ticket);
    auto old = snapshot(owner);
    const auto epoch = owner->getTopologyRevision().epoch;
    const auto oldId = owner->findLeaf({0, 0, 0});
    auto addedPoints = points(h, ops,
                              {
                                  {12.25f, .25f, .25f, 0}
    });
    ticket = prepareTopology(ops, addedPoints, g::PointTopologyMode::Accumulate);
    publish(h, owner, ticket);
    auto appended = snapshot(owner);
    require(owner->findLeaf({0, 0, 0}) == oldId && owner->getTopologyRevision().epoch == epoch &&
                appended->getDesc().channelMask == (1u << 17),
            "point append lost selected field or stable ID");
    ticket = prepareTopology(ops, originalPoints, g::PointTopologyMode::FullRebuild);
    publish(h, owner, ticket);
    auto subset = snapshot(owner);
    require(owner->getTopologyRevision().epoch > epoch && leaves(owner) ==
                                                              LeafSet{
                                                                  {0, 0, 0}
    },
            "point subset did not remove absent leaf");
    g::ReadbackHandle reads[3];
    g::IGVDBSnapshot* snapshots[] = {old.Get(), appended.Get(), subset.Get()};
    h.begin();
    for (uint32_t i = 0; i < 3; ++i) {
        check(owner->prepareReadback(snapshots[i], 1u << 17, &reads[i]), "prepare point channel readback");
        check(owner->recordReadback(h.context(), reads[i]), "record point channel readback");
    }
    owner = nullptr;
    ops = nullptr;
    ticket = nullptr;
    originalPoints = nullptr;
    addedPoints = nullptr;
    h.submit();
    for (uint32_t i = 0; i < 3; ++i) {
        check(reads[i]->finishAfterCallerCompletion(), "finish point channel after owner release");
        g::BorrowedBytes data;
        check(reads[i]->getChannelData(17, data), "get preserved field bytes");
        const uint32_t depth = i == 1 ? 12 : 6;
        require(data.byteCount == 6 * 6 * depth * 4, "point topology selected atlas dimensions");
        for (uint32_t z = 0; z < depth; ++z)
            for (uint32_t y = 0; y < 6; ++y)
                for (uint32_t x = 0; x < 6; ++x) {
                    const float expected =
                        x >= 1 && x <= 4 && y >= 1 && y <= 4 && z >= 1 && z <= 4 ? 77.f : -9.f;
                    float actual;
                    std::memcpy(&actual, static_cast<const uint8_t*>(data.data) + ((z * 6 + y) * 6 + x) * 4, 4);
                    require(actual == expected,
                            "point topology lost survivor interior or initialized new field incorrectly");
                }
    }
    std::puts(
        "PASS public point topology preserves selected owned field through append/subset and owner "
        "destruction");
}

void hostPoolLimits(Harness& h, const std::vector<char>& remap, const std::vector<char>& artifact) {
    g::VolumeCreateDesc create;
    create.device = h.device;
    create.configuration.levelCount = 5;
    create.configuration.logDimension = {2, 1, 1, 1, 1};
    create.maxHostPoolBytes = 256;  // Four 64-byte nodes or four 8-entry child lists per level.
    create.remapArtifact = remap.data();
    create.remapArtifactBytes = remap.size();
    g::VolumeHandle owner;
    check(g::createVolume(create, &owner), "create valid CPU-pool-limited owner");
    auto ops = makeOps(owner, artifact);
    check(owner->setChannel(0,
                            {
                                g::AtlasFormat::R32_FLOAT, {5, 0, 0, 0}
    }),
          "configure limited owner channel");
    auto original = points(h, ops,
                           {
                               {.25f, .25f, .25f, 0}
    });
    auto ticket = prepareTopology(ops, original, g::PointTopologyMode::FullRebuild);
    publish(h, owner, ticket);
    auto before = snapshot(owner);
    const auto revision = owner->getTopologyRevision();
    const auto root = owner->getRootId();
    const auto leaf = owner->findLeaf({0, 0, 0});
    std::array<std::vector<uint8_t>, 10> pools;
    for (uint32_t l = 0; l < 5; ++l)
        for (uint32_t group = 0; group < 2; ++group) {
            const auto pool = owner->getPool(group, l);
            auto& copy = pools[l * 2 + group];
            copy.resize(size_t(pool.count) * pool.stride);
            if (!copy.empty())
                std::memcpy(copy.data(), pool.data, copy.size());
        }
    const auto oversized = points(h, ops,
                                  {
                                      {.25f,   .25f, .25f, 0},
                                      {4.25f,  .25f, .25f, 0},
                                      {8.25f,  .25f, .25f, 0},
                                      {12.25f, .25f, .25f, 0},
                                      {16.25f, .25f, .25f, 0}
    });
    for (auto mode : {g::PointTopologyMode::FullRebuild, g::PointTopologyMode::Accumulate}) {
        g::PointTopologyDesc desc;
        desc.mode = mode;
        g::PlacementDesc placement;
        placement.minimumBrickCount = {1, 1, 1};
        auto* rejected = reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
        require(
            ops->prepareTopology(oversized, desc, placement, {}, &rejected) == FE_OUT_OF_MEMORY && !rejected,
            "point candidate growth ignored owner CPU pool limit");
        auto after = snapshot(owner);
        require(owner->getRootId() == root &&
                    owner->findLeaf({
                        0, 0, 0
        }) == leaf &&
                    leaves(owner) == LeafSet{{0, 0, 0}} &&
                    owner->getTopologyRevision().epoch == revision.epoch &&
                    owner->getTopologyRevision().topology == revision.topology &&
                    after->getDesc().revision.placement == before->getDesc().revision.placement,
                "failed CPU staging changed published topology");
        g::ChannelSnapshotDesc a, b;
        check(before->getChannel(0, a), "old limited channel");
        check(after->getChannel(0, b), "new limited channel");
        require(a.raw.buffer == b.raw.buffer && a.revision.resource == b.revision.resource &&
                    a.revision.content == b.revision.content,
                "failed CPU staging replaced published channel");
        for (uint32_t l = 0; l < 5; ++l)
            for (uint32_t group = 0; group < 2; ++group) {
                const auto pool = owner->getPool(group, l);
                const auto& expected = pools[l * 2 + group];
                require(pool.count * pool.stride == expected.size() &&
                            (expected.empty() || !std::memcmp(pool.data, expected.data(), expected.size())),
                        "failed CPU staging changed live pool contents");
            }
    }
    std::puts(
        "PASS public point candidate host-pool budgets preserve live topology, channels and output on "
        "full/append failure");
}

void lifetimes(Harness& h, const std::vector<char>& remap, const std::vector<char>& artifact) {
    auto owner = makeVolume(h.device, remap);
    auto ops = makeOps(owner, artifact);
    auto* borrowed = ops->getVolume();
    owner = nullptr;
    require(ops->getVolume() == borrowed, "point factory must retain its volume");
    std::vector<g::Float4> input{
        {1, 2, 3, 4},
        {5, 6, 7, 8}
    };
    g::PointSetDesc desc;
    desc.positions.stream = {input.data(), input.size() * 16,          0, 16,
                             input.size(), g::PointComponent::Float32, 4};
    g::PointUploadTicketHandle ticket;
    check(ops->preparePoints(desc, {}, &ticket), "prepare retained upload");
    h.begin();
    check(ops->recordPoints(h.context(), ticket), "record retained upload");
    ops = nullptr;
    std::fill(input.begin(), input.end(), g::Float4{});
    h.submit();
    g::PointSetHandle set;
    check(ticket->finishAfterCallerCompletion(&set), "finish after factory and volume destruction");
    ticket = nullptr;
    const float exact[] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint8_t> expected(sizeof(exact));
    std::memcpy(expected.data(), exact, sizeof(exact));
    verifyData(h, set, g::PointAttribute::Position, expected, 4, g::PointComponent::Float32);
    auto nextOwner = makeVolume(h.device, remap);
    auto nextOps = makeOps(nextOwner, artifact);
    auto generation = prepareTopology(nextOps, set, g::PointTopologyMode::FullRebuild);
    set = nullptr;
    nextOps = nullptr;
    publish(h, nextOwner, generation);
    require(leaves(nextOwner) ==
                LeafSet{
                    {0, 0, 0},
                    {4, 4, 4}
    },
            "completed point set must transfer between same-device factories");
    std::puts("PASS public point upload/set/factory lifetimes and same-device dataset reuse");
}
std::vector<char> readArtifact(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "open artifact");
    return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
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
            Harness h(device.device);
            const auto type = donut::app::GetShaderTypeName(device.device->getGraphicsAPI());
            auto remap = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_owner" / type /
                         "GVDBAtlasRepack_relocateAtlas.bin";
            auto conversion = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_slang" / type /
                              "GVDBPointStreamConversion_convertPointStream.bin";
            for (int i = 1; i + 1 < argc; ++i) {
                if (!std::strcmp(argv[i], "--owner-artifact"))
                    remap = argv[i + 1];
                if (!std::strcmp(argv[i], "--conversion-artifact"))
                    conversion = argv[i + 1];
            }
            const auto remapBytes = readArtifact(remap), conversionBytes = readArtifact(conversion);
            auto owner = makeVolume(device.device, remapBytes);
            auto ops = makeOps(owner, conversionBytes);
            queryIdentity(ops.Get());
            auto foreign = MAKE_RC_OBJ_PTR(gvdb_api_test::ForeignVolume);
            g::PointOpsCreateDesc invalid;
            invalid.volume = foreign;
            invalid.conversionArtifact = conversionBytes.data();
            invalid.conversionArtifactBytes = conversionBytes.size();
            auto* out = reinterpret_cast<g::IGVDBPointOps*>(uintptr_t(1));
            require(g::createPointOps(invalid, &out) == FE_INVALID_ARGS && !out,
                    "point factory accepted unrelated public volume implementation");
            conversions(h, ops);
            precisionAndColors(h, ops);
            invalidInputs(h, ops, conversionBytes);
            topologyGoldens(h, remapBytes, conversionBytes);
            retainedChannels(h, remapBytes, conversionBytes);
            hostPoolLimits(h, remapBytes, conversionBytes);
            lifetimes(h, remapBytes, conversionBytes);
        }
        require(problems == 0, "validation warnings/errors");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}

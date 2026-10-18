// SPDX-License-Identifier: Apache-2.0
// Public-interface fixture: never includes concrete HostVolume/NvrhiVolume headers.
#include "GVDBNvrhiResampleApi.h"
#include "GVDBNvrhiSerializer.h"
#include "slang/GVDBPortableVbx.h"
#include "../../tests/PortableFixtures.h"
#include "GVDBNvrhiForeignVolume.h"
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <nvrhi/core/foundation.h>
#include <nvrhi/core/datablob.h>
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

namespace s = gvdb::slang;
g::VolumeHandle makeVolume(IDevice* device, const std::vector<char>& remap) {
    g::VolumeCreateDesc desc;
    desc.device = device;
    desc.configuration.levelCount = 3;
    desc.configuration.logDimension = {1, 1, 1};
    desc.remapArtifact = remap.data();
    desc.remapArtifactBytes = remap.size();
    g::VolumeHandle result;
    check(g::createVolume(desc, &result), "create resample volume");
    return result;
}
g::SnapshotHandle snapshot(g::IGVDBVolume* volume) {
    g::SnapshotHandle result;
    check(volume->getSnapshot(&result), "get resample snapshot");
    return result;
}

struct Layout {
    g::UInt3 grid{2, 2, 3};
    uint32_t apron = 2;
    std::vector<std::pair<g::Int3, uint32_t>> leaves{
        {{-2, 0, 0}, 8},
        {{0, 0, 0},  0}
    };
    std::vector<uint32_t> bits;
};
uint32_t flat(const Layout& layout, uint32_t slot, uint32_t x, uint32_t y, uint32_t z) {
    const uint32_t pitch = 2 + 2 * layout.apron;
    const uint32_t ax = (slot % layout.grid.x) * pitch + x;
    const uint32_t ay = ((slot / layout.grid.x) % layout.grid.y) * pitch + y;
    const uint32_t az = (slot / (layout.grid.x * layout.grid.y)) * pitch + z;
    return (az * layout.grid.y * pitch + ay) * layout.grid.x * pitch + ax;
}
uint32_t bitsOf(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}
Layout originalLayout() {
    Layout result;
    result.bits.resize(12 * 12 * 18);
    for (uint32_t i = 0; i < result.bits.size(); ++i)
        result.bits[i] = i % 3 == 0   ? 0x7fc12345u + (i & 31)
                         : i % 3 == 1 ? 0x80000000u
                                      : bitsOf(float(i) * .25f - 5.f);
    return result;
}
s::VbxRawVolume rawFixture(const Layout& layout) {
    auto source = s::fixtures::twoBrickVolume();
    source.volume.atlasResolutionAndApron = {12, 12, 18, 2};
    source.volume.atlasBrickCountAndWidth = {2, 2, 3, 6};
    source.volume.boundsMin = {-2, 0, 0, 0};
    source.volume.boundsMax = {2, 2, 2, 0};
    source.nodes[1] = uint32_t(-2);
    source.nodes[17] = 0;
    source.nodes[33] = uint32_t(-2);
    source.nodes[36] = source.nodes[37] = source.nodes[38] = UINT32_MAX;
    source.nodes[4] = 2;
    source.nodes[5] = 2;
    source.nodes[6] = 14;
    source.nodes[20] = 2;
    source.nodes[21] = 2;
    source.nodes[22] = 2;
    source.atlasMap.assign(12, s::AtlasNode{-1, -1, -1, -1});
    source.atlasMap[8] = {-2, 0, 0, 0};
    source.atlasMap[0] = {0, 0, 0, 1};
    s::VbxRawVolume result;
    std::string error;
    require(s::packVolume(s::fixtures::volumeSource(source), result.packed, error), error.c_str());
    const s::AtlasFormat formats[] = {s::AtlasFormat::R32_FLOAT, s::AtlasFormat::R8_UINT,
                                      s::AtlasFormat::RGBA8_UINT, s::AtlasFormat::RGB32_FLOAT,
                                      s::AtlasFormat::RGBA32_FLOAT};
    for (auto format : formats) {
        s::VbxChannel channel;
        channel.format = format;
        channel.bytes.resize(12 * 12 * 18 * s::atlasStride(format), 0xeb);
        result.channels.push_back(std::move(channel));
    }
    std::memcpy(result.channels[0].bytes.data(), layout.bits.data(), layout.bits.size() * 4);
    return result;
}
void import(Harness& h, g::IGVDBVolume* volume, const s::VbxRawVolume& raw) {
    std::vector<uint8_t> bytes;
    std::string error;
    require(s::saveRawVBX(raw, bytes, error), error.c_str());
    AutoPtr<IDataBlob> blob;
    check(CreateBlob(bytes.size(), &blob), "create VBX input blob");
    std::memcpy(blob->GetDataPtr(), bytes.data(), bytes.size());
    g::SerializerCreateDesc desc;
    desc.volume = volume;
    g::SerializerHandle serializer;
    check(g::createSerializer(desc, &serializer), "create public fixture importer");
    g::GenerationTicketHandle ticket;
    check(serializer->prepareLoadVBX(blob, {}, &ticket), "prepare non-dense resample source");
    h.begin();
    check(volume->recordAndPublish(h.context(), ticket), "publish exact resample fixture");
    h.submit();
}
struct Dense {
    g::UInt3 size{5, 5, 3};
    std::vector<float> values;
    Dense() {
        for (uint32_t z = 0; z < size.z; ++z)
            for (uint32_t y = 0; y < size.y; ++y)
                for (uint32_t x = 0; x < size.x; ++x)
                    values.push_back(float(x + 10 * y + 100 * z) * .25f);
    }
};
struct OracleRequest {
    g::Float4 rows[3]{
        {1, 0, 0, .5f},
        {0, 1, 0, .5f},
        {0, 0, 1, .5f}
    };
    g::Float4 remap{0, 8, -1, 3};
    bool preserve = true;
    float background = -.625f;
};
Layout oracle(const Layout& previous, const Dense& dense, const OracleRequest& request) {
    Layout result = previous;
    std::fill(result.bits.begin(), result.bits.end(), 0);
    const uint32_t pitch = 2 + 2 * result.apron;
    for (const auto& leaf : result.leaves)
        for (uint32_t z = 0; z < pitch; ++z)
            for (uint32_t y = 0; y < pitch; ++y)
                for (uint32_t x = 0; x < pitch; ++x) {
                    const uint32_t index = flat(result, leaf.second, x, y, z);
                    uint32_t bits = request.preserve ? previous.bits[index] : bitsOf(request.background);
                    const int32_t coordinates[] = {leaf.first.x + int32_t(x) - int32_t(result.apron),
                                                   leaf.first.y + int32_t(y) - int32_t(result.apron),
                                                   leaf.first.z + int32_t(z) - int32_t(result.apron)};
                    double transformed[3];
                    bool valid = true;
                    int32_t point[3]{};
                    for (uint32_t axis = 0; axis < 3; ++axis) {
                        const auto row = request.rows[axis];
                        // Fixtures use exact binary fractions, or a single extreme term.
                        // Double evaluation independently classifies huge values before cast.
                        transformed[axis] = double(row.x) * coordinates[0] + double(row.y) * coordinates[1] +
                                            double(row.z) * coordinates[2] + row.w;
                        if (!std::isfinite(transformed[axis]) || transformed[axis] < -2147483648.0 ||
                            transformed[axis] >= 2147483648.0)
                            valid = false;
                        else
                            point[axis] = int32_t(std::trunc(transformed[axis]));
                    }
                    valid = valid && point[0] >= 0 && point[1] >= 0 && point[2] >= 0 &&
                            uint32_t(point[0]) < dense.size.x && uint32_t(point[1]) < dense.size.y &&
                            uint32_t(point[2]) < dense.size.z;
                    if (valid) {
                        const float input =
                            dense.values[(uint32_t(point[2]) * dense.size.y + uint32_t(point[1])) *
                                             dense.size.x +
                                         uint32_t(point[0])];
                        const float remapped = request.remap.z + (input - request.remap.x) *
                                                                     (request.remap.w - request.remap.z) /
                                                                     (request.remap.y - request.remap.x);
                        bits = bitsOf(remapped);
                    }
                    result.bits[index] = bits;
                }
    return result;
}
g::ReadbackHandle recordRead(Harness& h, g::IGVDBVolume* owner, g::IGVDBSnapshot* source, uint32_t channel) {
    g::ReadbackHandle result;
    check(owner->prepareReadback(source, 1u << channel, &result), "prepare resample readback");
    check(owner->recordReadback(h.context(), result), "record resample readback");
    return result;
}
void verify(g::IGVDBReadback* readback, uint32_t channel, const Layout& expected, const char* label) {
    check(readback->finishAfterCallerCompletion(), "finish resample readback");
    g::BorrowedBytes bytes;
    check(readback->getChannelData(channel, bytes), "get resampled raw bits");
    require(bytes.byteCount == expected.bits.size() * 4, "resample logical byte count");
    if (bytes.byteCount && std::memcmp(bytes.data, expected.bits.data(), bytes.byteCount)) {
        for (size_t i = 0; i < expected.bits.size(); ++i) {
            uint32_t actual;
            std::memcpy(&actual, static_cast<const uint8_t*>(bytes.data) + i * 4, 4);
            if (actual != expected.bits[i]) {
                std::fprintf(stderr, "%s voxel%zu actual%08x expected%08x\n", label, i, actual,
                             expected.bits[i]);
                break;
            }
        }
        throw std::runtime_error(label);
    }
}

g::ResampleHandle resample(g::IGVDBVolume* volume, const std::vector<char>& artifact) {
    g::ResampleCreateDesc desc;
    desc.volume = volume;
    desc.artifact = artifact.data();
    desc.artifactBytes = artifact.size();
    g::ResampleHandle result;
    check(g::createResample(desc, &result), "create public resample operator");
    return result;
}
g::ResampleRequestDesc descriptor(const Dense& dense, const OracleRequest& settings, uint32_t channel = 0) {
    g::ResampleRequestDesc result;
    result.channel = channel;
    result.sourceData = dense.values.data();
    result.sourceBytes = dense.values.size() * 4;
    result.sourceResolution = dense.size;
    for (uint32_t i = 0; i < 3; ++i)
        result.transformRows[i] = settings.rows[i];
    result.remap = settings.remap;
    result.preserveOutside = settings.preserve ? 1 : 0;
    result.background.x = settings.background;
    return result;
}
g::GenerationTicketHandle prepare(g::IGVDBResample* operation, const Dense& dense,
                                  const OracleRequest& settings, uint32_t channel = 0,
                                  g::ResamplePreparationBudget budget = {}) {
    g::GenerationTicketHandle result;
    check(operation->prepare(descriptor(dense, settings, channel), budget, &result), "prepare public resample");
    return result;
}
void publish(Harness& h, g::IGVDBVolume* volume, g::IGVDBGenerationTicket* ticket) {
    h.begin();
    check(volume->recordAndPublish(h.context(), ticket), "record public resample generation");
    h.submit();
}
void checkUnchanged(g::IGVDBSnapshot* before, g::IGVDBSnapshot* after) {
    const auto a = before->getDesc(), b = after->getDesc();
    require(a.revision.epoch == b.revision.epoch && a.revision.topology == b.revision.topology &&
                a.revision.placement == b.revision.placement && a.channelMask == b.channelMask,
            "failed preparation changed topology or mask");
    for (uint32_t ch = 0; ch < 32; ++ch)
        if (a.channelMask & (1u << ch)) {
            g::ChannelSnapshotDesc x, y;
            check(before->getChannel(ch, x), "old channel");
            check(after->getChannel(ch, y), "new channel");
            require(x.raw.buffer == y.raw.buffer && x.raw.byteOffset == y.raw.byteOffset &&
                        x.raw.byteSize == y.raw.byteSize && x.revision.resource == y.revision.resource &&
                        x.revision.content == y.revision.content,
                    "failed preparation changed channel");
        }
}
void checkMutation(g::IGVDBSnapshot* before, g::IGVDBSnapshot* after, uint32_t changed) {
    const auto a = before->getDesc(), b = after->getDesc();
    require(a.revision.epoch == b.revision.epoch && a.revision.topology == b.revision.topology &&
                a.revision.placement == b.revision.placement && a.channelMask == b.channelMask,
            "resample changed topology/placement/mask");
    for (auto kind : {g::TopologyBuffer::Nodes, g::TopologyBuffer::Children, g::TopologyBuffer::Levels,
                      g::TopologyBuffer::AtlasMap}) {
        g::BorrowedBufferBinding x, y;
        check(before->getTopologyBuffer(kind, x), "old topology binding");
        check(after->getTopologyBuffer(kind, y), "new topology binding");
        require(x.buffer == y.buffer && x.byteOffset == y.byteOffset && x.byteSize == y.byteSize,
                "resample replaced topology buffers");
    }
    for (uint32_t ch = 0; ch < 32; ++ch)
        if (a.channelMask & (1u << ch)) {
            g::ChannelSnapshotDesc x, y;
            check(before->getChannel(ch, x), "old channel");
            check(after->getChannel(ch, y), "new channel");
            if (ch == changed)
                require(x.raw.buffer != y.raw.buffer && y.revision.resource == x.revision.resource + 1 &&
                            y.revision.content == x.revision.content + 1,
                        "resample did not replace selected channel exactly once");
            else
                require(x.raw.buffer == y.raw.buffer && x.revision.resource == y.revision.resource &&
                            x.revision.content == y.revision.content,
                        "resample changed unselected channel");
        }
}
void run(IDevice* device, const std::vector<char>& remap, const std::vector<char>& artifact) {
    Harness h(device);
    auto owner = makeVolume(device, remap);
    const Layout source = originalLayout();
    import(h, owner, rawFixture(source));
    auto original = snapshot(owner);
    auto operation = resample(owner, artifact);
    queryIdentity(operation.Get());
    require(operation->getVolume() == owner.Get(), "operator retains exact public owner");
    require(
        original->getDesc().activeLeafCount == 2 && original->getDesc().volume.atlasBrickCountAndWidth.z == 3,
        "fixture requires signed non-dense reversed slots");
    Dense dense;
    const Dense untouched = dense;
    OracleRequest settings;
    const auto request = descriptor(dense, settings);
    auto rejects = [&](const g::ResampleRequestDesc& desc, g::ResamplePreparationBudget budget = {}) {
        auto before = snapshot(owner);
        auto* output = reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
        require(NVRHI_FAILED(operation->prepare(desc, budget, &output)) && !output,
                "invalid resample accepted or output retained");
        checkUnchanged(before, snapshot(owner));
    };
    require(operation->prepare(request, {}, nullptr) == FE_INVALID_ARGS, "null generation output");
    auto bad = request;
    bad.structSize--;
    rejects(bad);
    bad = request;
    bad.reserved = 1;
    rejects(bad);
    bad = request;
    bad.reserved2 = 1;
    rejects(bad);
    bad = request;
    bad.preserveOutside = 2;
    rejects(bad);
    for (uint32_t ch : {1u, 2u, 3u, 4u, 30u, 32u}) {
        bad = request;
        bad.channel = ch;
        rejects(bad);
    }
    bad = request;
    bad.sourceData = nullptr;
    rejects(bad);
    bad = request;
    --bad.sourceBytes;
    rejects(bad);
    bad = request;
    bad.sourceResolution.y = 0;
    rejects(bad);
    bad = request;
    bad.sourceResolution = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    rejects(bad);
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    for (uint32_t row = 0; row < 3; ++row)
        for (uint32_t component = 0; component < 4; ++component) {
            bad = request;
            (&bad.transformRows[row].x)[component] = component & 1 ? nan : inf;
            rejects(bad);
        }
    for (uint32_t component = 0; component < 4; ++component) {
        bad = request;
        (&bad.remap.x)[component] = nan;
        rejects(bad);
        bad = request;
        (&bad.background.x)[component] = inf;
        rejects(bad);
    }
    bad = request;
    bad.remap.y = bad.remap.x;
    rejects(bad);
    bad = request;
    bad.remap = {-FLT_MAX, FLT_MAX, 0, 1};
    rejects(bad);
    bad = request;
    bad.remap = {0, 1, -FLT_MAX, FLT_MAX};
    rejects(bad);
    g::ResamplePreparationBudget budget;
    budget.reserved = 1;
    rejects(request, budget);
    budget = {};
    budget.maxBuffers = 0;
    rejects(request, budget);
    budget = {};
    budget.maxAllocatedBytes = 0;
    rejects(request, budget);
    // Three fresh buffers: selected atlas, copied dense source and padded cbuffer.
    budget = {};
    budget.maxBuffers = 2;
    rejects(request, budget);
    budget = {};
    budget.maxAllocatedBytes = source.bits.size() * 4 + dense.values.size() * 4 + 255;
    rejects(request, budget);
    budget.maxAllocatedBytes++;
    budget.maxBuffers = 3;
    auto ticket = prepare(operation, dense, settings, 0, budget);
    queryIdentity(ticket.Get());
    require(ticket->getDevice() == device, "generation retains device");
    checkUnchanged(original, snapshot(owner));
    std::fill(dense.values.begin(), dense.values.end(),
              -1234.f);  // No caller storage may be used during recording.
    auto foreignOwner = makeVolume(device, remap);
    auto foreign = MAKE_RC_OBJ_PTR(gvdb_api_test::ForeignVolume);
    g::ResampleCreateDesc create;
    create.volume = foreign;
    create.artifact = artifact.data();
    create.artifactBytes = artifact.size();
    auto* foreignOutput = reinterpret_cast<g::IGVDBResample*>(uintptr_t(1));
    require(NVRHI_FAILED(g::createResample(create, &foreignOutput)) && !foreignOutput,
            "unrelated public owner accepted");
    h.begin();
    require(NVRHI_FAILED(foreignOwner->recordAndPublish(h.context(), ticket)),
            "foreign owner accepted generation");
    require(NVRHI_FAILED(owner->recordAndPublish({nullptr, h.commands}, ticket)),
            "wrong recording device accepted");
    require(NVRHI_FAILED(owner->recordAndPublish({device, nullptr}, ticket)), "null recording list accepted");
    check(owner->recordAndPublish(h.context(), ticket), "record copied dense source");
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(), ticket)), "generation recorded twice");
    auto first = snapshot(owner);
    checkMutation(original, first, 0);
    auto firstRead = recordRead(h, owner, first, 0);
    auto oldRead = recordRead(h, owner, original, 0);
    h.submit();
    Layout current = oracle(source, untouched, settings);
    verify(firstRead, 0, current, "affine truncation/deep copy/poison preservation");
    verify(oldRead, 0, source, "old raw imported allocation remains exact");
    dense = untouched;
    // The x=-1 sample transforms to -0.5 and must truncate to zero, not floor to -1.
    require(current.bits[flat(current, 8, 3, 2, 2)] == bitsOf(-1), "independent negative fractional reference");
    auto runCase = [&](const OracleRequest& value, const char* label) {
        auto before = snapshot(owner);
        auto pending = prepare(operation, dense, value);
        publish(h, owner, pending);
        auto after = snapshot(owner);
        checkMutation(before, after, 0);
        current = oracle(current, dense, value);
        h.begin();
        auto read = recordRead(h, owner, after, 0);
        h.submit();
        verify(read, 0, current, label);
    };
    settings.rows[0] = {0, .5f, .5f, .25f};
    settings.rows[1] = {-1, 0, 0, 1.5f};
    settings.rows[2] = {0, 0, .5f, .25f};
    settings.preserve = false;
    runCase(settings, "arbitrary affine axes and explicit outside background");
    // Values around the signed cast domain must be classified before conversion.
    for (float constant : {-2147483648.f, 2147483648.f, 2147483520.f, -1.f, -.75f}) {
        settings.rows[0] = {0, 0, 0, constant};
        settings.rows[1] = settings.rows[2] = {0, 0, 0, 0};
        settings.preserve = true;
        runCase(settings, "signed int32 cast domain and truncation");
    }
    settings.rows[0] = {FLT_MAX, 0, 0, 0};
    settings.rows[1] = settings.rows[2] = {0, 0, 0, 0};
    settings.preserve = false;
    runCase(settings, "finite transform coefficient producing nonfinite coordinates");
    // Input IEEE values are accepted and follow the documented remap arithmetic.
    // Assert classification/sign, not NaN payloads or arithmetic signed-zero bits.
    settings.rows[0] = settings.rows[1] = settings.rows[2] = {0, 0, 0, 0};
    settings.remap = {0, 1, 0, 1};
    for (float value : {nan, inf, -inf}) {
        dense.values[0] = value;
        auto ieeeTicket = prepare(operation, dense, settings);
        publish(h, owner, ieeeTicket);
        h.begin();
        auto read = recordRead(h, owner, snapshot(owner), 0);
        h.submit();
        check(read->finishAfterCallerCompletion(), "finish input IEEE resample");
        g::BorrowedBytes bytes;
        check(read->getChannelData(0, bytes), "read input IEEE results");
        require(bytes.byteCount == source.bits.size() * 4, "input IEEE atlas byte count");
        std::vector<uint8_t> active(source.bits.size(), 0);
        for (const auto& leaf : source.leaves)
            for (uint32_t z = 0; z < 6; ++z)
                for (uint32_t y = 0; y < 6; ++y)
                    for (uint32_t x = 0; x < 6; ++x)
                        active[flat(source, leaf.second, x, y, z)] = 1;
        for (size_t i = 0; i < active.size(); ++i) {
            uint32_t bits;
            std::memcpy(&bits, static_cast<const uint8_t*>(bytes.data) + i * 4, 4);
            if (!active[i])
                require(bits == 0, "input IEEE polluted unused slots");
            else {
                float actual;
                std::memcpy(&actual, &bits, 4);
                require(std::isnan(value) ? std::isnan(actual)
                                          : std::isinf(actual) && std::signbit(actual) == std::signbit(value),
                        "input IEEE remap classification/sign");
            }
        }
    }
    dense = untouched;
    // New high-bit channel starts from its void value; the old channel must retain its exact
    // resource/revisions.
    check(owner->setChannel(31,
                            {
                                g::AtlasFormat::R32_FLOAT, {-7.5f, 0, 0, 0}
    }),
          "configure high-bit channel");
    g::GenerationDesc generation;
    generation.placement.minimumBrickCount = {3, 1, 1};
    generation.placement.apron = 1;
    check(owner->prepareGeneration(generation, &ticket), "repack and initialize channel31");
    publish(h, owner, ticket);
    auto highBefore = snapshot(owner);
    Layout high;
    high.grid = {3, 1, 1};
    high.apron = 1;
    high.leaves = {
        {{-2, 0, 0}, 0},
        {{0, 0, 0},  1}
    };
    high.bits.assign(12 * 4 * 4, 0);
    for (const auto& leaf : high.leaves)
        for (uint32_t z = 0; z < 4; ++z)
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x)
                    high.bits[flat(high, leaf.second, x, y, z)] = bitsOf(-7.5f);
    h.begin();
    auto highInitial = recordRead(h, owner, highBefore, 31);
    h.submit();
    verify(highInitial, 31, high, "channel31 initialized interior/apron");
    settings = OracleRequest{};
    ticket = prepare(operation, dense, settings, 31);
    publish(h, owner, ticket);
    auto highAfter = snapshot(owner);
    checkMutation(highBefore, highAfter, 31);
    high = oracle(high, dense, settings);
    h.begin();
    auto highRead = recordRead(h, owner, highAfter, 31);
    h.submit();
    verify(highRead, 31, high, "repacked channel31 resample");
    // A later publication invalidates an already prepared ticket before it records any commands.
    auto staleBase = prepare(operation, dense, settings, 31);
    auto winner = prepare(operation, dense, settings, 31);
    publish(h, owner, winner);
    auto winnerSnapshot = snapshot(owner);
    h.begin();
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(), staleBase)), "stale base accepted");
    h.submit();
    checkUnchanged(winnerSnapshot, snapshot(owner));
    auto staleHost = prepare(operation, dense, settings, 31);
    const auto leaf = owner->findLeaf({0, 0, 0});
    require(owner->editNode(leaf) != nullptr, "borrow node to dirty host");
    check(owner->finishTopology(), "finish host edit");
    h.begin();
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(), staleHost)), "stale host accepted");
    h.submit();
    checkUnchanged(winnerSnapshot, snapshot(owner));
    check(owner->prepareGeneration(generation, &ticket), "resynchronize host edit");
    publish(h, owner, ticket);
    auto staleConfiguration = prepare(operation, dense, settings, 31);
    check(owner->setChannel(30, {g::AtlasFormat::R32_FLOAT, {}}), "change channel configuration");
    h.begin();
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(), staleConfiguration)),
            "stale channel configuration accepted");
    h.submit();
    check(owner->removeChannel(30), "restore configured channel mask");
    // Empty atlas still validates descriptors but needs no allocations. It is one-use and authenticated.
    check(owner->clearTopology(), "clear before empty resample");
    check(owner->finishTopology(), "finish empty host");
    check(owner->prepareGeneration(generation, &ticket), "prepare empty publication");
    publish(h, owner, ticket);
    budget = {};
    budget.maxAllocatedBytes = 0;
    budget.maxBuffers = 0;
    auto emptyTicket = prepare(operation, dense, settings, 31, budget);
    auto emptyBefore = snapshot(owner);
    bad = descriptor(dense, settings, 31);
    bad.sourceData = nullptr;
    rejects(bad, budget);
    h.begin();
    check(owner->recordAndPublish(h.context(), emptyTicket), "record empty resample");
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(), emptyTicket)), "empty ticket recorded twice");
    auto emptyAfter = snapshot(owner);
    checkUnchanged(emptyBefore, emptyAfter);
    auto emptyRead = recordRead(h, owner, emptyAfter, 31);
    // Read retained old/new allocations after destroying every public owner/operator/ticket before submission.
    auto retainedOld = recordRead(h, owner, original, 0);
    auto retainedNew = recordRead(h, owner, highAfter, 31);
    operation = nullptr;
    owner = nullptr;
    foreignOwner = nullptr;
    ticket = nullptr;
    emptyTicket = nullptr;
    staleBase = nullptr;
    staleHost = nullptr;
    staleConfiguration = nullptr;
    winner = nullptr;
    h.submit();
    Layout empty;
    empty.bits.clear();
    verify(emptyRead, 31, empty, "empty atlas readback");
    verify(retainedOld, 0, source, "retained old allocation after owner release");
    verify(retainedNew, 31, high, "retained new allocation after owner release");
    std::puts(
        "PASS public resample: signed non-dense slots, affine/truncation/cast guards, exact preserved bits, "
        "deep copy, channel31, stale/foreign/budget/empty/lifetimes");
}
std::vector<char> readArtifact(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "open resample artifact");
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
            const auto type = donut::app::GetShaderTypeName(device.device->getGraphicsAPI());
            const auto base = donut::app::GetDirectoryWithExecutable() / "shaders";
            auto remap = base / "gvdb_owner" / type / "GVDBAtlasRepack_relocateAtlas.bin";
            auto artifact = base / "gvdb_resample" / type / "GVDBAtlasResample_resampleOwnedAtlas.bin";
            for (int i = 1; i + 1 < argc; ++i) {
                if (!std::strcmp(argv[i], "--owner-artifact"))
                    remap = argv[i + 1];
                if (!std::strcmp(argv[i], "--resample-artifact"))
                    artifact = argv[i + 1];
            }
            run(device.device, readArtifact(remap), readArtifact(artifact));
        }
        require(problems == 0, "validation warnings/errors");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}

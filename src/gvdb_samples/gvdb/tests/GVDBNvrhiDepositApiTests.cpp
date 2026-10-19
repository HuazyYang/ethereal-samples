// SPDX-License-Identifier: Apache-2.0
// Public-interface fixture: never includes concrete HostVolume/NvrhiVolume headers.
#include "GVDBNvrhiDepositApi.h"
#include <ShaderToolArtifact.h>
#include <functional>
#include "GVDBNvrhiSerializer.h"
#include "GVDBNvrhiPointApi.h"
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
            std::printf("Deposit backend %s; adapter %d (%s): %s\n",
                        donut::app::GetShaderTypeName(device->getGraphicsAPI()), adapter,
                        adapter < 0 ? "automatic" : "explicit", manager->GetRendererString());
        else
            std::printf("Deposit backend CUDA; device ordinal %d\n", adapter < 0 ? 0 : adapter);
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
    desc.configuration.logDimension = {1, 2, 1};
    desc.remapArtifact = remap.data();
    desc.remapArtifactBytes = remap.size();
    g::VolumeHandle result;
    check(g::createVolume(desc, &result), "create gather volume");
    return result;
}
g::SnapshotHandle snapshot(g::IGVDBVolume* volume) {
    g::SnapshotHandle result;
    check(volume->getSnapshot(&result), "get gather snapshot");
    return result;
}

struct Leaf {
    g::Int3 origin;
    uint32_t slot;
};
struct Layout {
    g::UInt3 grid{2, 2, 3};
    uint32_t width = 2, apron = 2, leafPoolCount = 3;
    std::vector<Leaf> leaves{
        {{-2, 0, 0}, 8},
        {{2, 0, 0},  0}
    };
    std::vector<uint32_t> scalar, color;
};
uint32_t bitsOf(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}
float fromBits(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
uint32_t flat(const Layout& layout, uint32_t slot, uint32_t x, uint32_t y, uint32_t z) {
    const uint32_t pitch = layout.width + 2 * layout.apron;
    const uint32_t ax = (slot % layout.grid.x) * pitch + x,
                   ay = ((slot / layout.grid.x) % layout.grid.y) * pitch + y,
                   az = (slot / (layout.grid.x * layout.grid.y)) * pitch + z;
    return (az * layout.grid.y * pitch + ay) * layout.grid.x * pitch + ax;
}
void poison(Layout& result) {
    const uint32_t pitch = result.width + 2 * result.apron;
    result.scalar.resize(result.grid.x * result.grid.y * result.grid.z * pitch * pitch * pitch);
    result.color.resize(result.scalar.size());
    for (uint32_t i = 0; i < result.scalar.size(); ++i) {
        result.scalar[i] = i % 5 == 0   ? 0x7fc12345u + (i & 31)
                           : i % 5 == 1 ? 0x80000000u
                           : i % 5 == 2 ? bitsOf(8)
                           : i % 5 == 3 ? bitsOf(-8)
                                        : bitsOf(.5f);
        result.color[i] = ((i * 13u + 3) & 255u) | (((i * 7u + 11) & 255u) << 8) |
                          (((i * 3u + 71) & 255u) << 16) | (((i * 5u + 47) & 255u) << 24);
    }
}
Layout originalLayout(g::Int3 shift = {}) {
    Layout result;
    for (auto& leaf : result.leaves) {
        leaf.origin.x += shift.x;
        leaf.origin.y += shift.y;
        leaf.origin.z += shift.z;
    }
    poison(result);
    for (size_t i = 0; i < result.scalar.size(); ++i)
        result.scalar[i] = bitsOf(float(int(i % 17) - 8) * .125f);
    return result;
}
s::VbxRawVolume rawFixture(const Layout& layout) {
    s::fixtures::SparseVolume source;
    source.volume.atlasResolutionAndApron = {12, 12, 18, 2};
    source.volume.atlasBrickCountAndWidth = {2, 2, 3, 6};
    source.volume.hierarchy = {1, 0, 2, 0};
    const auto low = layout.leaves[0].origin, high = layout.leaves[1].origin;
    source.volume.boundsMin = {float(low.x), float(low.y), float(low.z), 0};
    source.volume.boundsMax = {float(high.x + 2), float(high.y + 2), float(high.z + 2), 0};
    source.levels = {
        {{0, 64, 3, 1},   {0, 0, 0, 2},   {1, 1, 1, 0}, {2, 2, 2, 0}},
        {{192, 64, 1, 2}, {0, 512, 1, 4}, {4, 4, 4, 0}, {8, 8, 8, 0}}
    };
    source.nodes.assign(64, 0);
    source.children.assign(128, UINT32_MAX);
    for (uint32_t i = 0; i < 4; ++i) {
        auto* node = source.nodes.data() + i * 16;
        const bool root = i == 3, active = i != 1;
        node[0] = (active ? 0x100 : 0) | (root ? 1 : 0);
        node[1] = uint32_t(low.x + (root ? 0 : int32_t(i * 2)));
        node[2] = uint32_t(low.y);
        node[3] = uint32_t(low.z);
        node[4] = node[5] = node[6] = UINT32_MAX;
        const auto parent = root ? s::UInt2{UINT32_MAX, UINT32_MAX} : s::makeReference(0, 1, 0);
        const auto children = root ? s::makeReference(1, 1, 0) : s::UInt2{UINT32_MAX, UINT32_MAX};
        node[10] = parent.x;
        node[11] = parent.y;
        node[12] = children.x;
        node[13] = children.y;
        if (!root) {
            const auto ref = s::makeReference(0, 0, i);
            source.children[2 * i] = ref.x;
            source.children[2 * i + 1] = ref.y;
        }
    }
    source.nodes[4] = source.nodes[5] = 2;
    source.nodes[6] = 14;
    source.nodes[36] = source.nodes[37] = source.nodes[38] = 2;
    source.atlasMap.assign(12, s::AtlasNode{-1, -1, -1, -1});
    source.atlasMap[8] = {low.x, low.y, low.z, 0};
    source.atlasMap[0] = {high.x, high.y, high.z, 2};
    s::VbxRawVolume result;
    std::string error;
    require(s::packVolume(s::fixtures::volumeSource(source), result.packed, error), error.c_str());
    for (auto format : {s::AtlasFormat::R32_FLOAT, s::AtlasFormat::R32_FLOAT, s::AtlasFormat::RGBA8_UINT,
                        s::AtlasFormat::R8_UINT, s::AtlasFormat::RGB32_FLOAT, s::AtlasFormat::RGBA32_FLOAT}) {
        s::VbxChannel channel;
        channel.format = format;
        channel.bytes.resize(layout.scalar.size() * s::atlasStride(format), 0xeb);
        result.channels.push_back(std::move(channel));
    }
    std::memcpy(result.channels[0].bytes.data(), layout.scalar.data(), layout.scalar.size() * 4);
    std::memcpy(result.channels[2].bytes.data(), layout.color.data(), layout.color.size() * 4);
    return result;
}
void import(Harness& h, g::IGVDBVolume* volume, const Layout& layout) {
    auto raw = rawFixture(layout);
    std::vector<uint8_t> bytes;
    std::string error;
    require(s::saveRawVBX(raw, bytes, error), error.c_str());
    AutoPtr<IDataBlob> blob;
    check(CreateBlob(bytes.size(), &blob), "create gather VBX blob");
    std::memcpy(blob->GetDataPtr(), bytes.data(), bytes.size());
    g::SerializerCreateDesc desc;
    desc.volume = volume;
    g::SerializerHandle serializer;
    check(g::createSerializer(desc, &serializer), "create gather fixture importer");
    g::GenerationTicketHandle ticket;
    check(serializer->prepareLoadVBX(blob, {}, &ticket), "prepare non-dense gather source");
    h.begin();
    check(volume->recordAndPublish(h.context(), ticket), "publish gather import");
    h.submit();
}
struct Input {
    std::vector<g::Float4> positions;
    std::vector<uint32_t> colors;
    std::vector<g::Float4> directions;
};
Input cloud(g::Int3 shift = {}) {
    Input result{
        {{-1.75f, .25f, .25f, 7},
         {.25f, .25f, .25f, -9},
         {2.75f, .25f, .25f, 11},
         {-.25f, 1.75f, .25f, -13},
         {100, 100, 100, 17}                                                        },
        {0x401d65adu,             0x912fab3bu, 0xe75d25c3u, 0x278f4337u, 0xff0000ffu}
    };
    result.positions.front().w = std::numeric_limits<float>::quiet_NaN();
    for (auto& p : result.positions) {
        p.x += shift.x;
        p.y += shift.y;
        p.z += shift.z;
    }
    return result;
}
g::PointOpsHandle pointOps(g::IGVDBVolume* volume, const std::vector<char>& artifact) {
    g::PointOpsCreateDesc desc;
    desc.volume = volume;
    desc.conversionArtifact = artifact.data();
    desc.conversionArtifactBytes = artifact.size();
    g::PointOpsHandle result;
    check(g::createPointOps(desc, &result), "create point conversion factory");
    return result;
}
g::PointSetHandle upload(Harness& h, g::IGVDBPointOps* ops, const Input& input, uint32_t components = 3,
                         bool colors = true) {
    // Deliberately unaligned/interleaved source records; gathered IDs must keep original colors.
    const uint32_t stride = components * 4 + 11;
    std::vector<uint8_t> bytes(input.positions.size() * stride + 5, 0xe7);
    for (size_t i = 0; i < input.positions.size(); ++i) {
        std::memcpy(bytes.data() + i * stride + 1, &input.positions[i], components * 4);
        if (colors)
            std::memcpy(bytes.data() + i * stride + components * 4 + 3, &input.colors[i], 4);
    }
    g::PointSetDesc desc;
    desc.positions.stream = {
        bytes.data(), bytes.size(), 1, stride, input.positions.size(), g::PointComponent::Float32, components};
    if (colors && !input.positions.empty())
        desc.colors.stream = {bytes.data(),
                              bytes.size(),
                              components * 4 + 3,
                              stride,
                              input.positions.size(),
                              g::PointComponent::UInt8,
                              4};
    if (!input.directions.empty())
        desc.velocities.stream = {input.directions.data(), input.directions.size() * 16, 0, 16,
                                  input.directions.size(), g::PointComponent::Float32,   4};
    g::PointUploadTicketHandle ticket;
    check(ops->preparePoints(desc, {}, &ticket), "prepare immutable gather points");
    h.begin();
    check(ops->recordPoints(h.context(), ticket), "record gather point conversion");
    h.submit();
    g::PointSetHandle result;
    check(ticket->finishAfterCallerCompletion(&result), "finish immutable gather points");
    return result;
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

struct Expected {
    Layout layout;
    std::vector<uint8_t> material, touched;
    uint64_t contributions = 0;
};
// Independent reference: visit every physical destination voxel, then inspect the
// original input sequence. No topology traversal, CSR construction, or GPU bins.
Expected oracle(const Layout& source, const Input& points, const g::DepositRequestDesc& request) {
    Expected out;
    out.layout = source;
    out.material.assign(source.scalar.size(), 0xeb);
    out.touched.assign(source.scalar.size(), 0);
    std::vector<uint8_t> active(source.scalar.size(), 0);
    const int pitch = int(source.width + 2 * source.apron);
    for (const auto& leaf : source.leaves)
        for (int z = 0; z < pitch; ++z)
            for (int y = 0; y < pitch; ++y)
                for (int x = 0; x < pitch; ++x) {
                    const auto index = flat(source, leaf.slot, x, y, z);
                    active[index] = 1;
                    double value = fromBits(source.scalar[index]);
                    uint64_t rgb[3] = {}, count = 0;
                    for (size_t ordinal = 0; ordinal < points.positions.size(); ++ordinal) {
                        const auto p = points.positions[ordinal];
                        const float base[] = {p.x + request.translation.x, p.y + request.translation.y,
                                              p.z + request.translation.z};
                        const int origin[] = {leaf.origin.x, leaf.origin.y, leaf.origin.z};
                        bool associated = true;
                        for (unsigned axis = 0; axis < 3; ++axis)
                            associated &=
                                base[axis] >= origin[axis] && base[axis] < origin[axis] + int(source.width);
                        if (!associated)
                            continue;
                        double local[3];
                        int center[3];
                        bool inside = true;
                        const auto direction =
                            points.directions.empty() ? g::Float4{} : points.directions[ordinal];
                        const float velocity[] = {direction.x, direction.y, direction.z};
                        for (unsigned axis = 0; axis < 3; ++axis) {
                            volatile float displacement = velocity[axis] * request.directionOffset;
                            volatile float shifted =
                                base[axis] + (request.kind == g::DepositKind::SupportXZ ? displacement : 0.f);
                            local[axis] = float(shifted - float(origin[axis]));
                            inside &= local[axis] > -1 && local[axis] < source.width;
                            center[axis] = int(local[axis]);
                        }
                        if (!inside)
                            continue;
                        const int voxel[] = {x - int(source.apron), y - int(source.apron),
                                             z - int(source.apron)};
                        const int dx = std::abs(voxel[0] - center[0]), dy = std::abs(voxel[1] - center[1]),
                                  dz = std::abs(voxel[2] - center[2]);
                        const bool central = dx + dy + dz == 0;
                        const bool chosen = request.kind == g::DepositKind::Central ? central
                                            : request.kind == g::DepositKind::SixNeighbors
                                                ? dx + dy + dz <= 1
                                                : dx <= 1 && dy == 0 && dz <= 1;
                        if (!chosen)
                            continue;
                        ++out.contributions;
                        out.touched[index] = 1;
                        double squared = 0;
                        for (unsigned axis = 0; axis < 3; ++axis)
                            squared += (voxel[axis] - local[axis]) * (voxel[axis] - local[axis]);
                        const double oneMinus = 1 - squared / (double(request.radius) * request.radius);
                        value = float(value + oneMinus * oneMinus * oneMinus);
                        if (request.kind == g::DepositKind::SupportXZ)
                            out.material[index] = 1;
                        if (central && request.color != g::DepositColor::None) {
                            if (request.color == g::DepositColor::LastOrdinal)
                                out.layout.color[index] = points.colors[ordinal];
                            else {
                                for (unsigned c = 0; c < 3; ++c)
                                    rgb[c] += (points.colors[ordinal] >> (8 * c)) & 255;
                                ++count;
                            }
                        }
                    }
                    if (out.touched[index])
                        out.layout.scalar[index] = bitsOf(float(value));
                    if (count)
                        out.layout.color[index] = uint32_t(rgb[0] / count) | uint32_t(rgb[1] / count) << 8 |
                                                  uint32_t(rgb[2] / count) << 16 | 0xff000000u;
                }
    for (size_t i = 0; i < active.size(); ++i)
        if (!active[i]) {
            out.layout.scalar[i] = 0;
            if (request.color != g::DepositColor::None)
                out.layout.color[i] = 0;
            if (request.kind == g::DepositKind::SupportXZ)
                out.material[i] = 0;
        }
    return out;
}
g::ReadbackHandle read(Harness& h, g::IGVDBVolume* owner, g::IGVDBSnapshot* source) {
    g::ReadbackHandle result;
    check(owner->prepareReadback(source, 13, &result), "prepare deposition readback");
    h.begin();
    check(owner->recordReadback(h.context(), result), "record deposition readback");
    h.submit();
    check(result->finishAfterCallerCompletion(), "finish deposition readback");
    return result;
}
void verify(g::IGVDBReadback* readback, const Expected& expected, const char* label) {
    g::BorrowedBytes scalar, color, material;
    check(readback->getChannelData(0, scalar), "read scalar");
    check(readback->getChannelData(2, color), "read color");
    check(readback->getChannelData(3, material), "read material");
    require(scalar.byteCount == expected.layout.scalar.size() * 4 && color.byteCount == scalar.byteCount &&
                material.byteCount * 4 == scalar.byteCount,
            "channel sizes");
    for (size_t i = 0; i < expected.layout.scalar.size(); ++i) {
        uint32_t actual, rgba;
        std::memcpy(&actual, static_cast<const uint8_t*>(scalar.data) + i * 4, 4);
        std::memcpy(&rgba, static_cast<const uint8_t*>(color.data) + i * 4, 4);
        const auto reference = expected.layout.scalar[i];
        const bool good =
            actual == reference || (expected.touched[i] && std::isfinite(fromBits(actual)) &&
                                    std::abs(double(fromBits(actual)) - fromBits(reference)) <=
                                        2e-5 * std::max(1., std::abs(double(fromBits(reference)))));
        if (!good || rgba != expected.layout.color[i] ||
            static_cast<const uint8_t*>(material.data)[i] != expected.material[i]) {
            std::fprintf(stderr, "%s index %zu scalar %g/%g color %08x/%08x material %u/%u\n", label, i,
                         fromBits(actual), fromBits(reference), rgba, expected.layout.color[i],
                         static_cast<const uint8_t*>(material.data)[i], expected.material[i]);
            throw std::runtime_error(label);
        }
    }
}
g::DepositHandle factory(g::IGVDBVolume* owner, const std::vector<char>& artifact) {
    g::DepositCreateDesc desc;
    desc.volume = owner;
    desc.artifact = artifact.data();
    desc.artifactBytes = artifact.size();
    g::DepositHandle result;
    check(g::createDeposit(desc, &result), "create deposition");
    return result;
}
void publish(Harness& h, g::IGVDBVolume* owner, g::IGVDBGenerationTicket* ticket) {
    h.begin();
    check(owner->recordAndPublish(h.context(), ticket), "record deposition");
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(), ticket)), "ticket reuse rejected");
    h.submit();
}
void artifactFailures(g::IGVDBVolume* owner, const std::vector<char>& accepted,
                      const std::vector<char>& wrong) {
    auto reject = [&](const std::vector<char>& bytes) {
        g::DepositCreateDesc desc;
        desc.volume = owner;
        desc.artifact = bytes.data();
        desc.artifactBytes = bytes.size();
        auto* out = reinterpret_cast<g::IGVDBDeposit*>(uintptr_t(1));
        require(NVRHI_FAILED(g::createDeposit(desc, &out)) && !out,
                "reject incompatible artifact and clear output");
    };
    reject({});
    reject({'b', 'a', 'd'});
    reject(wrong);
    reject(rewriteArtifact(accepted, [](auto& e, auto&) { e.ThreadGroupSize[0] = 128; }));
    reject(rewriteArtifact(accepted, [](auto& e, auto&) { e.Name = e.Symbol = "wrongEntry"; }));
    reject(rewriteArtifact(accepted, [](auto&, auto& b) { b[0].ElementStride += 16; }));
    reject(rewriteArtifact(accepted, [](auto&, auto& b) {
        for (auto& x : b)
            if (x.Scope == ST_SCOPE_CONSTANT_BUFFER) {
                x.Offset += 4;
                break;
            }
    }));
    reject(rewriteArtifact(accepted, [](auto&, auto& b) {
        for (auto& x : b)
            if (x.Scope == ST_SCOPE_GLOBAL && x.Kind == ST_BINDING_RW_BYTE_ADDRESS_BUFFER) {
                x.RegisterIndex = 9;
                break;
            }
    }));
}
void run(IDevice* device, const std::vector<char>& remap, const std::vector<char>& conversion,
         const std::vector<char>& artifact) {
    Harness h(device);
    auto owner = makeVolume(device, remap);
    auto source = originalLayout();
    import(h, owner, source);
    auto operation = factory(owner, artifact);
    auto ops = pointOps(owner, conversion);
    queryIdentity(operation.Get());
    require(operation->getVolume() == owner, "factory retains exact owner");
    artifactFailures(owner, artifact, conversion);
    auto original = snapshot(owner);
    auto retained = read(h, owner, original);
    auto input = cloud();
    input.positions.push_back(input.positions[0]);
    input.colors.push_back(0x7f11ff02);
    input.positions.push_back({-1.125f, .75f, .875f, 0});
    input.colors.push_back(0x190000ff);
    input.directions.assign(input.positions.size(), {0, 0, 0, std::numeric_limits<float>::quiet_NaN()});
    input.directions[0] = {-1, 0, 0, 0};   // shifted local -.25 truncates to0 in original leaf
    input.directions[2] = {-4, 0, 0, 0};   // lands in the other leaf: still rejected for original leaf
    input.directions[3] = {0, .5f, 0, 0};  // shifted outside Y rejects
    auto points = upload(h, ops, input, 4);
    auto uncolored = upload(h, ops, input, 3, false);
    auto noDirections = input;
    noDirections.directions.clear();
    auto missingDirection = upload(h, ops, noDirections);
    g::DepositRequestDesc request;
    request.points = points;
    request.color = g::DepositColor::LastOrdinal;
    request.colorChannel = 2;
    request.radius = 1.5f;
    auto reject = [&](g::DepositRequestDesc bad, g::DepositPreparationBudget budget = {}) {
        auto* out = reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
        require(NVRHI_FAILED(operation->prepare(bad, budget, &out)) && !out,
                "reject preparation and clear output");
        g::ChannelSnapshotDesc before, after;
        check(original->getChannel(0, before), "original channel");
        check(snapshot(owner)->getChannel(0, after), "current channel");
        require(before.raw.buffer == after.raw.buffer && before.revision.resource == after.revision.resource &&
                    before.revision.content == after.revision.content,
                "failed prepare preserves publication");
    };
    auto bad = request;
    bad.structSize--;
    reject(bad);
    bad = request;
    bad.reserved = 1;
    reject(bad);
    bad = request;
    bad.reserved2 = 1;
    reject(bad);
    bad = request;
    bad.points = nullptr;
    reject(bad);
    bad = request;
    bad.points = uncolored;
    reject(bad);
    bad = request;
    bad.channel = 3;
    reject(bad);
    bad = request;
    bad.colorChannel = 1;
    reject(bad);
    bad = request;
    bad.colorChannel = 0;
    reject(bad);
    bad = request;
    bad.kind = g::DepositKind(3);
    reject(bad);
    bad = request;
    bad.color = g::DepositColor(3);
    reject(bad);
    bad = request;
    bad.translation.x = std::numeric_limits<float>::infinity();
    reject(bad);
    for (float radius : {0.f, -1.f, 1e-5f, 1e9f, std::numeric_limits<float>::quiet_NaN()}) {
        bad = request;
        bad.radius = radius;
        reject(bad);
    }
    bad = request;
    bad.directionOffset = 1;
    reject(bad);
    bad = request;
    bad.materialChannel = 3;
    reject(bad);
    auto foreign = MAKE_RC_OBJ_PTR(ForeignSet, device);
    bad = request;
    bad.points = foreign;
    reject(bad);
    auto foreignOwner = MAKE_RC_OBJ_PTR(gvdb_api_test::ForeignVolume);
    g::DepositCreateDesc create;
    create.volume = foreignOwner;
    create.artifact = artifact.data();
    create.artifactBytes = artifact.size();
    g::IGVDBDeposit* invalid = reinterpret_cast<g::IGVDBDeposit*>(uintptr_t(1));
    require(NVRHI_FAILED(g::createDeposit(create, &invalid)) && !invalid, "reject foreign owner");
    bad = request;
    bad.kind = g::DepositKind::SupportXZ;
    bad.color = g::DepositColor::None;
    bad.colorChannel = UINT32_MAX;
    bad.materialChannel = 3;
    bad.points = missingDirection;
    reject(bad);
    auto nanInput = input;
    nanInput.directions[0].x = std::numeric_limits<float>::quiet_NaN();
    auto nanPoints = upload(h, ops, nanInput);
    bad.points = nanPoints;
    reject(bad);
    g::DepositPreparationBudget budget;
    budget.reserved = 1;
    reject(request, budget);
    budget = {};
    budget.maxPoints = input.positions.size() - 1;
    reject(request, budget);
    budget = {};
    budget.maxContributions = 0;
    reject(request, budget);
    budget = {};
    budget.maxHostBytes = 0;
    reject(request, budget);
    budget = {};
    budget.maxBuffers = 0;
    reject(request, budget);
    budget = {};
    budget.maxAllocatedBytes = 0;
    reject(request, budget);
    uint32_t cases = 0;
    for (auto kind : {g::DepositKind::Central, g::DepositKind::SixNeighbors, g::DepositKind::SupportXZ})
        for (auto color : {g::DepositColor::None, g::DepositColor::LastOrdinal, g::DepositColor::AverageRGB}) {
            if (kind == g::DepositKind::SupportXZ && color != g::DepositColor::None)
                continue;
            for (bool empty : {false, true}) {
                import(h, owner, source);
                original = snapshot(owner);
                auto current = empty ? Input{} : input;
                auto uploaded = upload(h, ops, current, empty ? 3 : 4, color != g::DepositColor::None);
                request = {};
                request.points = uploaded;
                request.kind = kind;
                request.color = color;
                request.radius = 1.5f;
                request.colorChannel = color == g::DepositColor::None ? UINT32_MAX : 2;
                request.materialChannel = kind == g::DepositKind::SupportXZ ? 3 : UINT32_MAX;
                request.directionOffset = kind == g::DepositKind::SupportXZ ? .5f : 0.f;
                auto expected = oracle(source, current, request);
                budget = {};
                budget.maxContributions = expected.contributions;
                if (expected.contributions) {
                    auto low = budget;
                    --low.maxContributions;
                    reject(request, low);
                }
                const uint64_t pairs = std::max(uint64_t(1), expected.contributions),
                               voxels = source.scalar.size();
                uint64_t topologyBytes = 0;
                for (auto which : {g::TopologyBuffer::Nodes, g::TopologyBuffer::Children,
                                   g::TopologyBuffer::Levels, g::TopologyBuffer::AtlasMap}) {
                    g::BorrowedBufferBinding binding;
                    check(original->getTopologyBuffer(which, binding), "topology bytes");
                    topologyBytes += binding.byteSize;
                }
                budget.maxHostBytes = topologyBytes + source.leafPoolCount * 4 + voxels * 16 + 256 + pairs * 16;
                budget.maxAllocatedBytes =
                    voxels * (color != g::DepositColor::None      ? 16
                              : kind == g::DepositKind::SupportXZ ? 13
                                                                  : 12) +
                    pairs * 8 + 256 +
                    (color != g::DepositColor::None || kind == g::DepositKind::SupportXZ ? 4 : 8);
                budget.maxBuffers = 6;
                auto low = budget;
                --low.maxHostBytes;
                reject(request, low);
                low = budget;
                --low.maxAllocatedBytes;
                reject(request, low);
                low = budget;
                --low.maxBuffers;
                reject(request, low);
                g::GenerationTicketHandle ticket, stale;
                check(operation->prepare(request, budget, &ticket), "exact contribution budget");
                check(operation->prepare(request, budget, &stale), "prepare competing publication");
                auto alternate = makeVolume(device, remap);
                h.begin();
                require(NVRHI_FAILED(alternate->recordAndPublish(h.context(), ticket)), "foreign owner ticket");
                require(NVRHI_FAILED(owner->recordAndPublish({nullptr, h.commands}, ticket)),
                        "invalid recording context");
                h.submit();
                uploaded = nullptr;  // ticket bindings retain canonical source streams
                publish(h, owner, ticket);
                h.begin();
                require(NVRHI_FAILED(owner->recordAndPublish(h.context(), stale)), "stale base rejected");
                h.submit();
                verify(read(h, owner, snapshot(owner)), expected, "deposition scalar/color/material oracle");
                auto old = read(h, owner, original);
                Expected untouched;
                untouched.layout = source;
                untouched.touched.assign(source.scalar.size(), 0);
                untouched.material.assign(source.scalar.size(), 0xeb);
                verify(old, untouched, "retained publication exact bytes");
                ++cases;
            }
        }
    // Translation and ordinal collisions, with a different position stream layout.
    import(h, owner, source);
    auto translated = input;
    for (auto& p : translated.positions) {
        p.x -= 4;
        p.y += 2;
        p.z -= 3;
    }
    auto translatedPoints = upload(h, ops, translated, 3);
    request = {};
    request.points = translatedPoints;
    request.translation = {4, -2, 3, 0};
    request.color = g::DepositColor::AverageRGB;
    request.colorChannel = 2;
    request.kind = g::DepositKind::SixNeighbors;
    request.radius = .5f;
    auto expected = oracle(source, translated, request);
    g::GenerationTicketHandle ticket;
    check(operation->prepare(request, {}, &ticket), "translated narrow radius");
    translatedPoints = nullptr;
    ops = nullptr;
    operation = nullptr;
    publish(h, owner, ticket);
    verify(read(h, owner, snapshot(owner)), expected, "translation and unclamped negative polynomial");
    std::printf(
        "PASS deposition: %u mode/empty fixtures, signed sparse placement, physical aprons, duplicate colors, "
        "shifted support, budgets, stale/lifetimes\n",
        cases);
}

void rootLeaf(IDevice* device, const std::vector<char>& remap, const std::vector<char>& conversion,
              const std::vector<char>& artifact) {
    Harness h(device);
    g::VolumeCreateDesc create;
    create.device = device;
    create.configuration.levelCount = 1;
    create.configuration.logDimension = {1};
    create.remapArtifact = remap.data();
    create.remapArtifactBytes = remap.size();
    g::VolumeHandle owner;
    check(g::createVolume(create, &owner), "root owner");
    g::NodeId root;
    check(owner->allocateNode(0, {3, -5, 1}, true, root), "root allocation");
    check(owner->setRoot(root), "set root");
    check(owner->finishTopology(), "finish root");
    check(owner->setChannel(31, {g::AtlasFormat::R32_FLOAT, {}}), "scalar channel31");
    g::GenerationDesc generation;
    generation.placement.apron = 0;
    generation.placement.minimumBrickCount = {1, 1, 1};
    g::GenerationTicketHandle ticket;
    check(owner->prepareGeneration(generation, &ticket), "root generation");
    publish(h, owner, ticket);
    auto operation = factory(owner, artifact);
    auto ops = pointOps(owner, conversion);
    Input input;
    input.positions = {
        {3, -5, 1, 0}
    };
    auto points = upload(h, ops, input, 3, false);
    g::DepositRequestDesc request;
    request.points = points;
    request.channel = 31;
    check(operation->prepare(request, {}, &ticket), "central without apron");
    publish(h, owner, ticket);
    g::ReadbackHandle output;
    check(owner->prepareReadback(snapshot(owner), 1u << 31, &output), "read root");
    h.begin();
    check(owner->recordReadback(h.context(), output), "record root read");
    h.submit();
    check(output->finishAfterCallerCompletion(), "finish root read");
    g::BorrowedBytes bytes;
    check(output->getChannelData(31, bytes), "root bytes");
    require(bytes.byteCount == 32, "root dimensions");
    for (unsigned i = 0; i < 8; ++i) {
        float v;
        std::memcpy(&v, static_cast<const uint8_t*>(bytes.data) + i * 4, 4);
        require(v == (i == 0 ? 1.f : 0.f), "root central golden");
    }
    request.kind = g::DepositKind::SixNeighbors;
    auto* invalid = reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
    require(operation->prepare(request, {}, &invalid) == FE_UNSUPPORTED && !invalid,
            "expansion requires apron");
    request.kind = g::DepositKind::Central;
    check(operation->prepare(request, {}, &ticket), "prepare host-stale deposition");
    check(owner->clearTopology(), "clear root");
    check(owner->finishTopology(), "finish clear");
    h.begin();
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(), ticket)), "host-stale deposition rejected");
    h.submit();
    check(owner->prepareGeneration(generation, &ticket), "publish empty topology");
    publish(h, owner, ticket);
    g::DepositPreparationBudget budget;
    budget.maxContributions = budget.maxHostBytes = budget.maxAllocatedBytes = 0;
    budget.maxBuffers = 0;
    check(operation->prepare(request, budget, &ticket), "empty owner allocation-free ticket");
    publish(h, owner, ticket);
    std::puts("PASS root-only: channel31, zero apron, exact central value, host staleness, empty owner");
}
std::vector<char> artifact(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "open deposition artifact");
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
                artifact(base / "gvdb_slang" / target / "GVDBPointStreamConversion_convertPointStream.bin"),
                artifact(base / "gvdb_deposit" / target / "GVDBPointDeposit_depositOwnedPoints.bin"));
            rootLeaf(
                device.device, artifact(base / "gvdb_owner" / target / "GVDBAtlasRepack_relocateAtlas.bin"),
                artifact(base / "gvdb_slang" / target / "GVDBPointStreamConversion_convertPointStream.bin"),
                artifact(base / "gvdb_deposit" / target / "GVDBPointDeposit_depositOwnedPoints.bin"));
        }
        require(problems == 0, "validation warnings/errors");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}

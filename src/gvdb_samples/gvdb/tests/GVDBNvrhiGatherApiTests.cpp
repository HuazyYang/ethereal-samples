// SPDX-License-Identifier: Apache-2.0
// Public-interface fixture: never includes concrete HostVolume/NvrhiVolume headers.
#include "GVDBNvrhiGatherApi.h"
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
uint64_t acceptedColorBoundaries = 0;
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
            std::printf("Gather backend %s; adapter %d (%s): %s\n",
                        donut::app::GetShaderTypeName(device->getGraphicsAPI()), adapter,
                        adapter < 0 ? "automatic" : "explicit", manager->GetRendererString());
        else
            std::printf("Gather backend CUDA; device ordinal %d\n", adapter < 0 ? 0 : adapter);
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
    if (colors)
        desc.colors.stream = {bytes.data(),
                              bytes.size(),
                              components * 4 + 3,
                              stride,
                              input.positions.size(),
                              g::PointComponent::UInt8,
                              4};
    g::PointUploadTicketHandle ticket;
    check(ops->preparePoints(desc, {}, &ticket), "prepare immutable gather points");
    h.begin();
    check(ops->recordPoints(h.context(), ticket), "record gather point conversion");
    h.submit();
    g::PointSetHandle result;
    check(ticket->finishAfterCallerCompletion(&result), "finish immutable gather points");
    return result;
}
struct Settings {
    bool levelSet = false, accumulate = false, colors = true;
    uint32_t width = 2;
    float radius = 1;
    float supportRadius = -1;
    g::Float4 translation{};
};
struct Expected {
    Layout layout;
    std::vector<uint8_t> approximate;
    std::vector<std::array<uint16_t, 4>> colorLower, colorUpper;
    std::vector<uint8_t> colorInterval;
    uint64_t cells = 0, visits = 0, pairs = 0;
};
std::array<float, 3> translated(const g::Float4& p, const Settings& request) {
    return {p.x + request.translation.x, p.y + request.translation.y, p.z + request.translation.z};
}
std::array<int64_t, 3> cellOrigin(const Leaf& leaf, uint32_t x, uint32_t y, uint32_t z, uint32_t width) {
    return {int64_t(leaf.origin.x) + (x / width) * width, int64_t(leaf.origin.y) + (y / width) * width,
            int64_t(leaf.origin.z) + (z / width) * width};
}
bool selected(const std::array<int64_t, 3>& cell, const std::array<float, 3>& point, const Settings& request) {
    for (uint32_t axis = 0; axis < 3; ++axis) {
        // Cell origin is on the established lattice, so use its half-open spatial box
        // to classify endpoint cells without reproducing a production point-bin loop.
        const float support = request.supportRadius == -1 ? request.radius : request.supportRadius;
        const float lower = point[axis] - support, upper = (point[axis] + 1.f) + support;
        if (double(lower) >= double(cell[axis] + request.width) || double(upper) < double(cell[axis]))
            return false;
    }
    return true;
}
Expected oracle(const Layout& previous, const Input& input, const Settings& request, const Input* evaluation = nullptr) {
    Expected result;
    result.layout = previous;
    auto& out = result.layout;
    std::fill(out.scalar.begin(), out.scalar.end(), 0);
    if (request.colors)
        std::fill(out.color.begin(), out.color.end(), 0);
    result.approximate.assign(out.scalar.size(), 0);
    result.colorLower.resize(out.scalar.size());
    result.colorUpper.resize(out.scalar.size());
    result.colorInterval.assign(out.scalar.size(), 0);
    const uint32_t pitch = out.width + 2 * out.apron;
    const uint32_t side = out.width / request.width;
    result.cells = uint64_t(out.leafPoolCount) * side * side * side;
    for (const auto& leaf : out.leaves)
        for (uint32_t z = 0; z < pitch; ++z)
            for (uint32_t y = 0; y < pitch; ++y)
                for (uint32_t x = 0; x < pitch; ++x) {
                    const auto at = flat(out, leaf.slot, x, y, z);
                    const bool apron = x < out.apron || y < out.apron || z < out.apron ||
                                       x >= out.apron + out.width || y >= out.apron + out.width ||
                                       z >= out.apron + out.width;
                    if (apron) {
                        out.scalar[at] = previous.scalar[at];
                        out.color[at] = previous.color[at];
                        continue;
                    }
                    const uint32_t localX = x - out.apron, localY = y - out.apron, localZ = z - out.apron;
                    const auto cell = cellOrigin(leaf, localX, localY, localZ, request.width);
                    uint32_t scalar =
                        request.accumulate ? previous.scalar[at] : bitsOf(request.levelSet ? 3.f : 0.f);
                    float rgba[4] = {0, 0, 0, 1};
                    uint64_t byteSum[4] = {0, 0, 0, 255};
                    bool binaryLane[4] = {true, true, true, true};
                    if (request.accumulate)
                        for (uint32_t c = 0; c < 4; ++c) {
                            const uint32_t b = (previous.color[at] >> (8 * c)) & 255;
                            rgba[c] = float(b) / 255.f;
                            byteSum[c] = b;
                            binaryLane[c] = b == 0 || b == 255;
                        }
                    uint32_t count = 0;
                    for (size_t p = 0; p < input.positions.size(); ++p) {
                        if (!selected(cell, translated(input.positions[p], request), request))
                            continue;
                        const auto point = translated((evaluation ? evaluation->positions : input.positions)[p], request);
                        ++count;
                        const double dx = double(point[0]) - (leaf.origin.x + int32_t(localX)),
                                     dy = double(point[1]) - (leaf.origin.y + int32_t(localY)),
                                     dz = double(point[2]) - (leaf.origin.z + int32_t(localZ));
                        const float distance = float(std::sqrt(dx * dx + dy * dy + dz * dz));
                        const float candidate =
                            request.levelSet ? distance - request.radius : request.radius - distance;
                        if (request.levelSet ? candidate < fromBits(scalar) : candidate > fromBits(scalar)) {
                            scalar = bitsOf(candidate);
                            result.approximate[at] = 1;
                        }
                        if (request.colors)
                            for (uint32_t c = 0; c < 4; ++c) {
                                const uint32_t b = ((evaluation ? evaluation->colors : input.colors)[p] >> (8 * c)) & 255;
                                rgba[c] += float(b) / 255.f;
                                byteSum[c] += b;
                                binaryLane[c] = binaryLane[c] && (b == 0 || b == 255);
                            }
                    }
                    out.scalar[at] = scalar;
                    if (!request.colors)
                        out.color[at] = previous.color[at];
                    else if (!count)
                        out.color[at] = request.accumulate ? previous.color[at] : 0;
                    else {
                        uint32_t packed = 0;
                        for (uint32_t c = 0; c < 4; ++c) {
                            const float component = rgba[c] / float(count + (request.accumulate ? 1 : 0));
                            packed |= (uint32_t(component * 255.f) & 255u) << (8 * c);
                            const uint32_t denominator = count + (request.accumulate ? 1 : 0);
                            // Exact 0/1 normalized lanes and power-of-two division need
                            // no allowance (including the independent alpha510->254 case).
                            if (!(binaryLane[c] && !(denominator & (denominator - 1)))) {
                                // Nonnegative sums have no cancellation. Bound each float
                                // normalization/add/divide/multiply by one full epsilon;
                                // N additions plus eight rounding steps give gamma_(N+8).
                                // The pre-quantized component is at most510, so512*gamma
                                // bounds absolute byte-unit error. Only integer boundaries
                                // intersected by this interval permit an adjacent byte.
                                const double t = (double(count) + 8) * FLT_EPSILON;
                                const double error = 512 * t / (1 - t),
                                             ideal = double(byteSum[c]) / denominator;
                                const uint32_t lower = uint32_t(std::floor(std::max(0.0, ideal - error)));
                                const uint32_t upper = uint32_t(std::floor(ideal + error));
                                require(upper - lower <= 1, "fixture color interval must remain narrow");
                                result.colorLower[at][c] = uint16_t(lower);
                                result.colorUpper[at][c] = uint16_t(upper);
                                result.colorInterval[at] |= uint8_t(1u << c);
                            }
                        }
                        out.color[at] = packed;
                    }
                }
    // Independent bounded grid enumeration counts candidate visits, including holes,
    // and actual pairs. Inactive leaf pool entries still count toward maxCells above.
    if (!out.leaves.empty()) {
        auto low = out.leaves.front().origin, high = low;
        for (const auto& leaf : out.leaves) {
            low.x = std::min(low.x, leaf.origin.x);
            low.y = std::min(low.y, leaf.origin.y);
            low.z = std::min(low.z, leaf.origin.z);
            high.x = std::max(high.x, leaf.origin.x);
            high.y = std::max(high.y, leaf.origin.y);
            high.z = std::max(high.z, leaf.origin.z);
        }
        for (const auto& value : input.positions) {
            const auto point = translated(value, request);
            for (int64_t z = low.z; z < int64_t(high.z) + out.width; z += request.width)
                for (int64_t y = low.y; y < int64_t(high.y) + out.width; y += request.width)
                    for (int64_t x = low.x; x < int64_t(high.x) + out.width; x += request.width) {
                        if (!selected({x, y, z}, point, request))
                            continue;
                        ++result.visits;
                        for (const auto& leaf : out.leaves)
                            if (x >= leaf.origin.x && x < int64_t(leaf.origin.x) + out.width &&
                                y >= leaf.origin.y && y < int64_t(leaf.origin.y) + out.width &&
                                z >= leaf.origin.z && z < int64_t(leaf.origin.z) + out.width) {
                                ++result.pairs;
                                break;
                            }
                    }
        }
    }
    return result;
}
g::ReadbackHandle recordRead(Harness& h, g::IGVDBVolume* owner, g::IGVDBSnapshot* source, uint32_t mask = 5) {
    g::ReadbackHandle result;
    check(owner->prepareReadback(source, mask, &result), "prepare gather readback");
    check(owner->recordReadback(h.context(), result), "record gather readback");
    return result;
}
void verify(g::IGVDBReadback* readback, const Expected& expected, const char* label, uint32_t scalarChannel = 0,
            uint32_t colorChannel = 2) {
    check(readback->finishAfterCallerCompletion(), "finish gather readback");
    g::BorrowedBytes scalar, color;
    check(readback->getChannelData(scalarChannel, scalar), "get gathered scalar");
    if (colorChannel != UINT32_MAX)
        check(readback->getChannelData(colorChannel, color), "get gathered colors");
    require(scalar.byteCount == expected.layout.scalar.size() * 4 &&
                (colorChannel == UINT32_MAX || color.byteCount == expected.layout.color.size() * 4),
            "gather atlas byte count");
    for (size_t i = 0; i < expected.layout.scalar.size(); ++i) {
        uint32_t actual;
        std::memcpy(&actual, static_cast<const uint8_t*>(scalar.data) + 4 * i, 4);
        const auto reference = expected.layout.scalar[i];
        const bool good = actual == reference || (expected.approximate[i] && std::isfinite(fromBits(actual)) &&
                                                  std::abs(fromBits(actual) - fromBits(reference)) <=
                                                      2e-6f * std::max(1.f, std::abs(fromBits(reference))));
        if (!good) {
            std::fprintf(stderr, "%s scalar[%zu] %08x != %08x\n", label, i, actual, reference);
            throw std::runtime_error(label);
        }
        if (colorChannel != UINT32_MAX) {
            std::memcpy(&actual, static_cast<const uint8_t*>(color.data) + 4 * i, 4);
            for (uint32_t c = 0; c < 4; ++c) {
                const uint32_t lane = (actual >> (8 * c)) & 255,
                               referenceLane = (expected.layout.color[i] >> (8 * c)) & 255;
                bool accepted = lane == referenceLane;
                if (!expected.colorInterval.empty() && (expected.colorInterval[i] & (1u << c))) {
                    accepted = false;
                    for (uint32_t candidate = expected.colorLower[i][c]; candidate <= expected.colorUpper[i][c];
                         ++candidate)
                        accepted |= lane == (candidate & 255);
                }
                if (!accepted) {
                    std::fprintf(stderr, "%s color[%zu].%u %u != %u\n", label, i, c, lane, referenceLane);
                    throw std::runtime_error(label);
                }
                if (lane != referenceLane)
                    ++acceptedColorBoundaries;
            }
        }
    }
}
Expected exact(const Layout& layout) {
    Expected result;
    result.layout = layout;
    result.approximate.assign(layout.scalar.size(), 0);
    return result;
}

g::GatherHandle gather(g::IGVDBVolume* volume, const std::vector<char>& artifact) {
    g::GatherCreateDesc desc;
    desc.volume = volume;
    desc.artifact = artifact.data();
    desc.artifactBytes = artifact.size();
    g::GatherHandle result;
    check(g::createGather(desc, &result), "create public gather");
    return result;
}
g::GatherRequestDesc descriptor(g::IGVDBPointSet* points, const Settings& settings, uint32_t channel = 0,
                                uint32_t colorChannel = 2) {
    g::GatherRequestDesc desc;
    desc.points = points;
    desc.kind = settings.levelSet ? g::GatherKind::LevelSet : g::GatherKind::Density;
    desc.channel = channel;
    desc.colorChannel = settings.colors ? colorChannel : UINT32_MAX;
    desc.subcellWidth = settings.width;
    desc.radius = settings.radius;
    desc.supportRadius = settings.supportRadius;
    desc.accumulate = settings.accumulate ? 1 : 0;
    desc.translation = settings.translation;
    return desc;
}
g::GenerationTicketHandle prepare(g::IGVDBGather* operation, g::IGVDBPointSet* points, const Settings& settings,
                                  g::GatherPreparationBudget budget = {}, uint32_t channel = 0,
                                  uint32_t colorChannel = 2) {
    g::GenerationTicketHandle result;
    check(operation->prepare(descriptor(points, settings, channel, colorChannel), budget, &result),
          "prepare gather generation");
    return result;
}
void publish(Harness& h, g::IGVDBVolume* owner, g::IGVDBGenerationTicket* ticket) {
    h.begin();
    check(owner->recordAndPublish(h.context(), ticket), "record gather generation");
    h.submit();
}
void unchanged(g::IGVDBSnapshot* before, g::IGVDBSnapshot* after, uint32_t changedMask = 0) {
    const auto a = before->getDesc(), b = after->getDesc();
    require(a.revision.epoch == b.revision.epoch && a.revision.topology == b.revision.topology &&
                a.revision.placement == b.revision.placement && a.channelMask == b.channelMask,
            "gather changed topology/placement/channel mask");
    for (auto kind : {g::TopologyBuffer::Nodes, g::TopologyBuffer::Children, g::TopologyBuffer::Levels,
                      g::TopologyBuffer::AtlasMap}) {
        g::BorrowedBufferBinding x, y;
        check(before->getTopologyBuffer(kind, x), "old topology");
        check(after->getTopologyBuffer(kind, y), "new topology");
        require(x.buffer == y.buffer && x.byteOffset == y.byteOffset && x.byteSize == y.byteSize,
                "gather replaced topology");
    }
    for (uint32_t ch = 0; ch < 32; ++ch)
        if (a.channelMask & (1u << ch)) {
            g::ChannelSnapshotDesc x, y;
            check(before->getChannel(ch, x), "old channel");
            check(after->getChannel(ch, y), "new channel");
            if (changedMask & (1u << ch))
                require(x.raw.buffer != y.raw.buffer && y.revision.resource == x.revision.resource + 1 &&
                            y.revision.content == x.revision.content + 1,
                        "gather replacement revision");
            else
                require(x.raw.buffer == y.raw.buffer && x.revision.resource == y.revision.resource &&
                            x.revision.content == y.revision.content,
                        "gather changed unselected channel");
        }
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
void run(IDevice* device, const std::vector<char>& remap, const std::vector<char>& conversion,
         const std::vector<char>& artifact) {
    Harness h(device);
    auto owner = makeVolume(device, remap);
    const Layout source = originalLayout();
    import(h, owner, source);
    auto operation = gather(owner, artifact);
    auto ops = pointOps(owner, conversion);
    const Input input = cloud();
    auto points3 = upload(h, ops, input), points4 = upload(h, ops, input, 4),
         noColors = upload(h, ops, input, 3, false);
    queryIdentity(operation.Get());
    require(operation->getVolume() == owner.Get(), "gather retains exact owner");
    Settings settings;
    auto original = snapshot(owner);
    const auto request = descriptor(points3, settings);
    auto rejects = [&](const g::GatherRequestDesc& desc, g::GatherPreparationBudget budget = {}) {
        auto before = snapshot(owner);
        auto* output = reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
        require(NVRHI_FAILED(operation->prepare(desc, budget, &output)) && !output,
                "invalid gather accepted or output retained");
        unchanged(before, snapshot(owner));
    };
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
    bad.kind = g::GatherKind(2);
    rejects(bad);
    bad = request;
    bad.accumulate = 2;
    rejects(bad);
    for (uint32_t channel : {2u, 3u, 4u, 5u, 30u, 32u}) {
        bad = request;
        bad.channel = channel;
        rejects(bad);
    }
    for (uint32_t channel : {0u, 1u, 3u, 4u, 5u, 30u, 32u}) {
        bad = request;
        bad.colorChannel = channel;
        rejects(bad);
    }
    for (uint32_t width : {0u, 3u, 4u, 8u, 16u}) {
        bad = request;
        bad.subcellWidth = width;
        rejects(bad);
    }
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    for (float radius : {-1.f, 0.f, nan, inf}) {
        bad = request;
        bad.radius = radius;
        rejects(bad);
    }
    for (float support : {-2.f, -.5f, nan, inf}) {
        bad = request;
        bad.supportRadius = support;
        rejects(bad);
    }
    for (uint32_t i = 0; i < 4; ++i) {
        bad = request;
        (&bad.translation.x)[i] = nan;
        rejects(bad);
    }
    bad = request;
    bad.points = nullptr;
    rejects(bad);
    bad = request;
    bad.points = noColors;
    rejects(bad);
    auto foreignSet = MAKE_RC_OBJ_PTR(ForeignSet, device);
    bad = request;
    bad.points = foreignSet;
    rejects(bad);
    require(operation->prepare(request, {}, nullptr) == FE_INVALID_ARGS, "null gather output");
    auto foreignVolume = MAKE_RC_OBJ_PTR(gvdb_api_test::ForeignVolume);
    g::GatherCreateDesc create;
    create.volume = foreignVolume;
    create.artifact = artifact.data();
    create.artifactBytes = artifact.size();
    auto* foreignOperator = reinterpret_cast<g::IGVDBGather*>(uintptr_t(1));
    require(NVRHI_FAILED(g::createGather(create, &foreignOperator)) && !foreignOperator,
            "foreign public owner accepted");
    const auto firstExpected = oracle(source, input, settings);
    require(firstExpected.cells == 3 && firstExpected.visits == 9 && firstExpected.pairs == 5,
            "independent clipped hole/active pair golden");
    g::GatherPreparationBudget budget;
    budget.reserved = 1;
    rejects(request, budget);
    budget = {};
    budget.maxCells = 2;
    rejects(request, budget);
    budget = {};
    budget.maxCandidateVisits = 8;
    rejects(request, budget);
    budget = {};
    budget.maxPointPairs = 4;
    rejects(request, budget);
    budget = {};
    budget.maxBuffers = 4;
    rejects(request, budget);
    const uint64_t exactBytes =
        source.scalar.size() * 8 + firstExpected.cells * 8 + firstExpected.pairs * 4 + 256;
    budget = {};
    budget.maxAllocatedBytes = exactBytes - 1;
    rejects(request, budget);
    budget.maxAllocatedBytes = exactBytes;
    budget.maxBuffers = 5;
    budget.maxCells = 3;
    budget.maxCandidateVisits = 9;
    budget.maxPointPairs = 5;
    // Support controls only bin membership; its independently counted visits/pairs
    // must also govern CPU and GPU preparation budgets.
    for (float support : {2.f, 0.f}) {
        Settings options = settings;
        options.supportRadius = support;
        const auto expected = oracle(source, input, options);
        const uint64_t visits = support == 2 ? 10 : 5, pairs = support == 2 ? 6 : 3;
        require(expected.cells == 3 && expected.visits == visits && expected.pairs == pairs,
                "independent support budget golden");
        auto desc = descriptor(points3, options);
        g::GatherPreparationBudget limited;
        limited.maxCandidateVisits = visits - 1;
        rejects(desc, limited);
        limited = {};
        limited.maxPointPairs = pairs - 1;
        rejects(desc, limited);
        limited = {};
        limited.maxCells = 3;
        limited.maxCandidateVisits = visits;
        limited.maxPointPairs = pairs;
        limited.maxBuffers = 5;
        limited.maxAllocatedBytes = source.scalar.size() * 8 + 3 * 8 + pairs * 4 + 256;
        auto exactSupportTicket = prepare(operation, points3, options, limited);
        --limited.maxAllocatedBytes;
        rejects(desc, limited);
    }
    auto ticket = prepare(operation, points3, settings, budget);
    queryIdentity(ticket.Get());
    unchanged(original, snapshot(owner));
    auto foreignOwner = makeVolume(device, remap);
    h.begin();
    require(NVRHI_FAILED(foreignOwner->recordAndPublish(h.context(), ticket)),
            "foreign owner accepted gather ticket");
    require(NVRHI_FAILED(owner->recordAndPublish({nullptr, h.commands}, ticket)), "invalid context accepted");
    require(NVRHI_FAILED(owner->recordAndPublish({device, nullptr}, ticket)), "null list accepted");
    check(owner->recordAndPublish(h.context(), ticket), "record gathered points");
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(), ticket)), "gather ticket reused");
    auto first = snapshot(owner);
    unchanged(original, first, 5);
    auto firstRead = recordRead(h, owner, first);
    auto oldRead = recordRead(h, owner, original);
    h.submit();
    verify(firstRead, firstExpected, "fresh density stable color association");
    verify(oldRead, exact(source), "retained exact imported source");
    auto runCase = [&](const Input& values, g::IGVDBPointSet* points, const Settings& options,
                       const char* label, const Layout& initial) {
        import(h, owner, initial);
        auto before = snapshot(owner);
        auto expected = oracle(initial, values, options);
        auto pending = prepare(operation, points, options);
        publish(h, owner, pending);
        auto after = snapshot(owner);
        unchanged(before, after, options.colors ? 5u : 1u);
        h.begin();
        auto read = recordRead(h, owner, after);
        h.submit();
        verify(read, expected, label);
        return expected;
    };
    runCase(input, points4, settings, "Float4 canonical position matches Float3", source);
    settings.supportRadius = 2;
    auto widened = runCase(input, points3, settings, "distance1/support2 independent bin colors", source);
    require(widened.layout.scalar == firstExpected.layout.scalar &&
                widened.layout.color != firstExpected.layout.color,
            "wider support must change far-point color coverage without changing distance field");
    settings.supportRadius = 0;
    runCase(input, points3, settings, "support0 still follows point-plus-one cell coverage", source);
    settings.supportRadius = 1;
    auto explicitDefault =
        runCase(input, points3, settings, "explicit support1 matches radius1 sentinel", source);
    require(explicitDefault.layout.scalar == firstExpected.layout.scalar &&
                explicitDefault.layout.color == firstExpected.layout.color,
            "explicit default support changed oracle");
    settings.supportRadius = -1;

    settings.levelSet = true;
    settings.width = 1;
    settings.radius = .75f;
    settings.translation = {.5f, -.25f, .25f, 123};
    runCase(input, points3, settings, "translated levelset and inclusive upper support boundary", source);
    settings.accumulate = true;
    runCase(input, points4, settings, "levelset accumulation raw NaN/tie/seed colors", source);
    settings = Settings{};
    settings.accumulate = true;
    runCase(input, points3, settings, "density accumulation raw NaN/tie/seed colors", source);
    settings.colors = false;
    runCase(input, noColors, settings, "optional color leaves whole color allocation unchanged", source);
    // A point exactly one radius from a -0 seed produces +0 and must not replace the seed bits.
    Input tie;
    uint32_t tieIndex = UINT32_MAX;
    for (const auto& leaf : source.leaves)
        for (uint32_t z = 0; z < 2; ++z)
            for (uint32_t y = 0; y < 2; ++y)
                for (uint32_t x = 0; x < 2; ++x) {
                    const auto index = flat(source, leaf.slot, x + 2, y + 2, z + 2);
                    if (source.scalar[index] == 0x80000000u && tieIndex == UINT32_MAX) {
                        tieIndex = index;
                        tie.positions.push_back({float(leaf.origin.x + int32_t(x) + 1),
                                                 float(leaf.origin.y + int32_t(y)),
                                                 float(leaf.origin.z + int32_t(z)), 0});
                    }
                }
    require(tieIndex != UINT32_MAX, "negative-zero tie fixture seed");
    auto tieSet = upload(h, ops, tie, 3, false);
    auto tieExpected =
        runCase(tie, tieSet, settings, "ordered comparison preserves negative-zero ties", source);
    require(tieExpected.layout.scalar[tieIndex] == 0x80000000u, "independent tie remains negative zero");
    // A single opaque point adds fresh alpha1, producing 510 which wraps to byte254.
    Input alpha{{{-1, 0, 0, 0}}, {0xff0000ffu}};
    auto alphaSet = upload(h, ops, alpha);
    settings = Settings{};
    settings.radius = .25f;
    auto alphaExpected = runCase(alpha, alphaSet, settings, "fresh initial alpha and low-byte wrap", source);
    require((alphaExpected.layout.color[flat(source, 8, 2, 2, 2)] >> 24) == 254, "initial alpha wrap golden");
    // Nonempty points that miss the active bbox still reset fresh scalar/colors; accumulation keeps exact
    // interior bits.
    Input misses{{{100, 100, 100, 0}}, {0xff123456u}};
    auto missedSet = upload(h, ops, misses);
    settings.levelSet = true;
    runCase(misses, missedSet, settings, "zero contributing pairs reset fresh cells", source);
    settings.accumulate = true;
    runCase(misses, missedSet, settings, "zero contributing pairs preserve accumulated bits", source);
    Input emptyInput;
    auto emptyPoints = upload(h, ops, emptyInput, 3, false);
    settings.colors = false;
    settings.accumulate = false;
    runCase(emptyInput, emptyPoints, settings, "zero points reset fresh scalar", source);
    settings.accumulate = true;
    runCase(emptyInput, emptyPoints, settings, "zero points preserve scalar bits", source);
    // Shift all nodes off the zero-origin lattice. Signed phase, including Y/Z, must follow node origins.
    const g::Int3 shift{1, 1, -1};
    const auto shifted = originalLayout(shift);
    const auto shiftedInput = cloud(shift);
    auto shiftedSet = upload(h, ops, shiftedInput, 4);
    settings = Settings{};
    runCase(shiftedInput, shiftedSet, settings, "phase-relative cells for shifted imported topology", shifted);
    // Large stable lists exercise association and the actual input-index accumulation order.
    Input ordered;
    for (uint32_t i = 0; i < 257; ++i) {
        ordered.positions.push_back({-1, 0, 0, float(i)});
        ordered.colors.push_back((i * 17u + 9) & 255u | (((i * 31u + 43) & 255u) << 8) |
                                 (((i * 71u + 19) & 255u) << 16) | (((i * 11u + 3) & 255u) << 24));
    }
    auto orderedSet = upload(h, ops, ordered, 4);
    runCase(ordered, orderedSet, settings, "257-point stable color accumulation", source);
    // Nonfinite translated XYZ is rejected even though canonical point XYZ itself is finite.
    Input extreme{{{FLT_MAX, 0, 0, 0}}, {0xffffffffu}};
    auto extremeSet = upload(h, ops, extreme);
    bad = descriptor(extremeSet, settings);
    bad.translation.x = FLT_MAX;
    rejects(bad);
    for (uint32_t axis : {1u, 2u}) {
        Input outside{{{100, 0, 0, 0}}, {0xffffffffu}};
        (&outside.positions[0].x)[axis] = FLT_MAX;
        auto outsideSet = upload(h, ops, outside);
        bad = descriptor(outsideSet, settings);
        (&bad.translation.x)[axis] = FLT_MAX;
        rejects(bad);
    }
    // Independent scalar channel31 mutation must retain every other channel binding and revision.
    import(h, owner, source);
    check(owner->setChannel(31,
                            {
                                g::AtlasFormat::R32_FLOAT, {-2, 0, 0, 0}
    }),
          "configure gather channel31");
    g::GenerationDesc generation;
    generation.placement.minimumBrickCount = {3, 1, 1};
    generation.placement.apron = 1;
    check(owner->prepareGeneration(generation, &ticket), "prepare channel31 placement");
    publish(h, owner, ticket);
    Layout high;
    high.grid = {3, 1, 1};
    high.width = 2;
    high.apron = 1;
    high.leafPoolCount = 3;
    high.leaves = {
        {{-2, 0, 0}, 0},
        {{2, 0, 0},  1}
    };
    high.scalar.assign(12 * 4 * 4, 0);
    high.color.assign(high.scalar.size(), 0);
    for (const auto& leaf : high.leaves)
        for (uint32_t z = 0; z < 4; ++z)
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x)
                    high.scalar[flat(high, leaf.slot, x, y, z)] = bitsOf(-2);
    settings.colors = false;
    auto highBefore = snapshot(owner);
    ticket = prepare(operation, noColors, settings, {}, 31);
    publish(h, owner, ticket);
    auto highAfter = snapshot(owner);
    unchanged(highBefore, highAfter, 1u << 31);
    h.begin();
    auto highRead = recordRead(h, owner, highAfter, 1u << 31);
    h.submit();
    auto highExpected = oracle(high, input, settings);
    verify(highRead, highExpected, "high-bit scalar channel", 31, UINT32_MAX);
    auto staleBase = prepare(operation, noColors, settings, {}, 31),
         winner = prepare(operation, noColors, settings, {}, 31);
    publish(h, owner, winner);
    auto winning = snapshot(owner);
    h.begin();
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(), staleBase)), "stale gather base accepted");
    h.submit();
    unchanged(winning, snapshot(owner));
    auto staleHost = prepare(operation, noColors, settings, {}, 31);
    require(owner->editNode(owner->findLeaf({-2, 0, 0})) != nullptr, "dirty gather host");
    check(owner->finishTopology(), "finish dirty host");
    h.begin();
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(), staleHost)), "stale gather host accepted");
    h.submit();
    check(owner->prepareGeneration(generation, &ticket), "resync host");
    publish(h, owner, ticket);
    auto staleConfig = prepare(operation, noColors, settings, {}, 31);
    check(owner->setChannel(30, {g::AtlasFormat::R32_FLOAT, {}}), "change gather config");
    h.begin();
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(), staleConfig)), "stale gather config accepted");
    h.submit();
    check(owner->removeChannel(30), "restore mask");
    check(owner->clearTopology(), "clear gather volume");
    check(owner->finishTopology(), "finish empty gather");
    check(owner->prepareGeneration(generation, &ticket), "prepare empty gather generation");
    publish(h, owner, ticket);
    budget = {};
    budget.maxCells = budget.maxCandidateVisits = budget.maxPointPairs = budget.maxAllocatedBytes = 0;
    budget.maxBuffers = 0;
    auto emptyTicket = prepare(operation, noColors, settings, budget, 31);
    h.begin();
    check(owner->recordAndPublish(h.context(), emptyTicket), "record empty gather no-op");
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(), emptyTicket)), "empty gather ticket reused");
    h.submit();
    // Empty volume validates/authenticates the descriptor but never walks translated points.
    auto emptyExtremeSettings = settings;
    emptyExtremeSettings.translation.x = FLT_MAX;
    auto emptyExtreme = prepare(operation, extremeSet, emptyExtremeSettings, budget, 31);
    publish(h, owner, emptyExtreme);
    // Preparation retains PointSet GPU resources; release point factories and points before recording,
    // then owners/operators before submission. Old and new readbacks remain valid independently.
    import(h, owner, source);
    settings = Settings{};
    auto lifetimeInput = upload(h, ops, input, 4);
    auto lifetimeTicket = prepare(operation, lifetimeInput, settings);
    lifetimeInput = nullptr;
    ops = nullptr;
    points3 = nullptr;
    points4 = nullptr;
    noColors = nullptr;
    orderedSet = nullptr;
    h.begin();
    check(owner->recordAndPublish(h.context(), lifetimeTicket), "record after PointSet release");
    auto final = snapshot(owner);
    auto lifetimeRead = recordRead(h, owner, final), retainedRead = recordRead(h, owner, original);
    operation = nullptr;
    owner = nullptr;
    foreignOwner = nullptr;
    lifetimeTicket = nullptr;
    ticket = nullptr;
    emptyTicket = nullptr;
    h.submit();
    verify(lifetimeRead, oracle(source, input, settings), "point set and owner lifetime");
    verify(retainedRead, exact(source), "old gather source after owner release");
    std::puts(
        "PASS public ordinary gather: independent signed/shifted subcell oracle, stable point/color "
        "association, density/levelset/accumulation, empty/poison/apron policies, channel31, "
        "budgets/stale/lifetimes");
}
void evaluationGather(IDevice* device, const std::vector<char>& remap,
                      const std::vector<char>& conversion, const std::vector<char>& artifact) {
    Harness h(device);
    auto owner=makeVolume(device,remap);
    auto source=originalLayout(); import(h,owner,source);
    auto ops=pointOps(owner,conversion);
    auto operation=gather(owner,artifact);
    g::GatherEvaluationHandle extension;
    check(operation->QueryInterface(uuid_of<g::IGVDBGatherEvaluation>(),
        reinterpret_cast<void**>(extension.GetAddressOf())),"query evaluation extension");
    queryIdentity(extension.Get());
    g::GatherHandle base;
    check(extension->QueryInterface(uuid_of<g::IGVDBGather>(),
        reinterpret_cast<void**>(base.GetAddressOf())),"query original gather interface");
    require(base==operation,"extension preserves original interface identity");
    auto associations=cloud(), evaluations=associations;
    // Deliberately change cell membership and colors; binning evaluated data
    // would produce a different scalar/color result in both active leaves.
    for(auto& p:evaluations.positions) p.x+=4;
    std::reverse(evaluations.colors.begin(),evaluations.colors.end());
    auto original=upload(h,ops,associations,3,false);
    auto evaluated=upload(h,ops,evaluations,4);
    g::GatherEvaluationDesc desc;
    Settings settings;settings.levelSet=true;settings.width=1;settings.supportRadius=0;
    desc.request=descriptor(original,settings);desc.evaluationPoints=evaluated;
    auto reject=[&](g::GatherEvaluationDesc bad,g::GatherPreparationBudget budget={}) {
        auto before=snapshot(owner);
        auto* output=reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
        require(NVRHI_FAILED(extension->prepareEvaluation(bad,budget,&output))&&!output,
            "invalid evaluation rejected and output cleared");
        unchanged(before,snapshot(owner));
    };
    auto bad=desc;--bad.structSize;reject(bad);
    bad=desc;bad.reserved=1;reject(bad);
    bad=desc;bad.evaluationPoints=nullptr;reject(bad);
    bad=desc;bad.request.points=nullptr;reject(bad);
    auto shortInput=evaluations;shortInput.positions.pop_back();shortInput.colors.pop_back();
    auto shorter=upload(h,ops,shortInput);bad=desc;bad.evaluationPoints=shorter;reject(bad);
    bad=desc;bad.evaluationPoints=original;reject(bad); // Evaluation colors required.
    g::GatherPreparationBudget budget;budget.maxPointPairs=0;reject(desc,budget);
    require(extension->prepareEvaluation(desc,{},nullptr)==FE_INVALID_ARGS,"null evaluation output");
    auto expected=oracle(source,associations,settings,&evaluations);
    const auto rebinned=oracle(source,evaluations,settings);
    require(expected.layout.scalar!=rebinned.layout.scalar || expected.layout.color!=rebinned.layout.color,
        "cross-boundary fixture distinguishes accidental rebinning");
    g::GenerationTicketHandle ticket;
    check(extension->prepareEvaluation(desc,{},&ticket),"prepare separate evaluation");
    original=nullptr;evaluated=nullptr;extension=nullptr;operation=nullptr;base=nullptr;ops=nullptr;
    publish(h,owner,ticket);
    h.begin();auto read=recordRead(h,owner,snapshot(owner));h.submit();
    verify(read,expected,"original associations with separate distances and colors");
    std::puts("PASS separate gather association/evaluation, validation and lifetime");
}
void rootLeaf(IDevice* device, const std::vector<char>& remap, const std::vector<char>& conversion,
              const std::vector<char>& artifact, uint32_t logWidth) {
    Harness h(device);
    g::VolumeCreateDesc create;
    create.device = device;
    create.configuration.levelCount = 1;
    create.configuration.logDimension[0] = logWidth;
    create.remapArtifact = remap.data();
    create.remapArtifactBytes = remap.size();
    g::VolumeHandle owner;
    check(g::createVolume(create, &owner), "create root leaf owner");
    const g::Int3 origin{1, -3, 5};
    g::NodeId root;
    check(owner->allocateNode(0, origin, true, root), "allocate shifted root leaf");
    check(owner->setRoot(root), "set root-only leaf");
    check(owner->finishTopology(), "finish root-only topology");
    check(owner->setChannel(0, {g::AtlasFormat::R32_FLOAT, {}}), "root scalar0");
    check(owner->setChannel(2, {g::AtlasFormat::RGBA8_UINT, {}}), "root color2");
    Layout source;
    source.width = 1u << logWidth;
    source.grid = {2, 1, 1};
    source.apron = 2;
    source.leafPoolCount = 1;
    source.leaves = {
        {origin, 0}
    };
    poison(source);
    const uint32_t pitch = source.width + 4;
    std::vector<uint8_t> interior(source.scalar.size(), 0);
    for (uint32_t z = 2; z < source.width + 2; ++z)
        for (uint32_t y = 2; y < source.width + 2; ++y)
            for (uint32_t x = 2; x < source.width + 2; ++x)
                interior[flat(source, 0, x, y, z)] = 1;
    for (size_t i = 0; i < interior.size(); ++i)
        if (!interior[i])
            source.scalar[i] = source.color[i] = 0;
    g::ChannelUploadDesc uploads[] = {
        {0, 0, source.scalar.data(), source.scalar.size() * 4},
        {2, 0, source.color.data(),  source.color.size() * 4 }
    };
    g::GenerationDesc generation;
    generation.placement.minimumBrickCount = source.grid;
    generation.placement.apron = 2;
    generation.uploads = uploads;
    generation.uploadCount = 2;
    g::GenerationTicketHandle ticket;
    check(owner->prepareGeneration(generation, &ticket), "prepare root-only atlas");
    publish(h, owner, ticket);
    auto ops = pointOps(owner, conversion);
    auto operation = gather(owner, artifact);
    Input input{
        {{1.25f, -2.75f, 5.25f, 0}, {3.25f, -.75f, 7.25f, 99}},
        {0x6037a91du,               0xb3472971u              }
    };
    auto points = upload(h, ops, input, 4);
    Settings settings;
    settings.width = 2;
    settings.radius = 1.5f;
    auto expected = oracle(source, input, settings);
    ticket = prepare(operation, points, settings);
    publish(h, owner, ticket);
    h.begin();
    auto read = recordRead(h, owner, snapshot(owner));
    h.submit();
    verify(read, expected, "root-only shifted leaf subcells");
    source = expected.layout;
    // Seed subsequent accumulation with the independently validated prior bytes.
    g::BorrowedBytes validatedColors;
    check(read->getChannelData(2, validatedColors), "validated prior colors");
    std::memcpy(source.color.data(), validatedColors.data, source.color.size() * 4);
    settings.width = 1u << logWidth;
    settings.accumulate = true;
    settings.levelSet = true;
    expected = oracle(source, input, settings);
    ticket = prepare(operation, points, settings);
    publish(h, owner, ticket);
    h.begin();
    read = recordRead(h, owner, snapshot(owner));
    h.submit();
    verify(read, expected, "subcell widths4/8 and root-only accumulation");
}
std::vector<char> readArtifact(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "open gather artifact");
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
            auto conversion = base / "gvdb_slang" / type / "GVDBPointStreamConversion_convertPointStream.bin";
            auto artifact = base / "gvdb_gather" / type / "GVDBAtlasGather_gatherOwnedAtlas.bin";
            for (int i = 1; i + 1 < argc; ++i) {
                if (!std::strcmp(argv[i], "--owner-artifact"))
                    remap = argv[i + 1];
                if (!std::strcmp(argv[i], "--conversion-artifact"))
                    conversion = argv[i + 1];
                if (!std::strcmp(argv[i], "--gather-artifact"))
                    artifact = argv[i + 1];
            }
            const auto ownerBytes = readArtifact(remap), conversionBytes = readArtifact(conversion),
                       gatherBytes = readArtifact(artifact);
            run(device.device, ownerBytes, conversionBytes, gatherBytes);
            evaluationGather(device.device, ownerBytes, conversionBytes, gatherBytes);
            for (uint32_t logWidth : {2u, 3u})
                rootLeaf(device.device, ownerBytes, conversionBytes, gatherBytes, logWidth);
        }
        std::printf("Accepted derived color-boundary byte differences: %llu\n",
                    static_cast<unsigned long long>(acceptedColorBoundaries));
        require(problems == 0, "validation warnings/errors");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}

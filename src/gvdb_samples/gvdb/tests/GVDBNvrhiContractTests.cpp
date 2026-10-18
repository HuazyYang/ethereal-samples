// SPDX-License-Identifier: Apache-2.0
// Standalone CPU-only Step A contract fixture. No NVRHI binary or GPU SDK needed.
#include "../GVDBNvrhi.h"
#include <cstdio>
#include <limits>
#include <type_traits>
namespace g = gvdb::portable;
namespace {
bool require(bool value, const char* message) {
    if (!value) std::fprintf(stderr, "FAIL: %s\n", message);
    return value;
}
// Stack-owned, single-threaded descriptor mock. No resource/device allocation.
struct DescriptorBuffer final : nvrhi::IBuffer {
    nvrhi::BufferDesc desc;
    nvrhi::FLONG references = 1;
    nvrhi::FRESULT QueryInterface(nvrhi::FREFIID iid, void** output) noexcept override {
        if (output) *output = nullptr;
        if (iid != nvrhi::uuid_of<nvrhi::IBuffer>() && iid != nvrhi::uuid_of<nvrhi::IRHIObject>() &&
            iid != nvrhi::uuid_of<nvrhi::IObject>()) return nvrhi::FE_NOINTERFACE;
        if (output) { *output = static_cast<nvrhi::IBuffer*>(this); AddRef(); }
        return nvrhi::FS_OK;
    }
    nvrhi::FLONG AddRef() noexcept override { return ++references; }
    nvrhi::FLONG Release() noexcept override { return --references; }
    const nvrhi::BufferDesc& getDesc() const noexcept override { return desc; }
    nvrhi::GpuVirtualAddress getGpuVirtualAddress() const noexcept override { return 0; }
};
bool pointStreamDescriptors() {
    DescriptorBuffer buffer;
    buffer.desc.byteSize = 64;
    buffer.desc.canHaveRawViews = true;
    bool ok = true;
    {
        g::PointStream points;
        points.buffer = &buffer;
        points.byteOffset = 4; points.byteStride = 16; points.count = 2;
        ok &= require(bool(g::validatePointStream(points)), "canonical raw point stream accepts explicit stride");
        buffer.desc.structStride = 16;
        ok &= require(!g::validatePointStream(points), "point stream rejects simultaneous raw/structured descriptor");
        buffer.desc.structStride = 0;
        buffer.desc.canHaveRawViews = false;
        ok &= require(!g::validatePointStream(points), "point stream requires raw-view support");
        buffer.desc.canHaveRawViews = true;
        buffer.desc.isVolatile = true;
        ok &= require(!g::validatePointStream(points), "point stream rejects volatile constant allocation");
        buffer.desc.isVolatile = false;
        points.byteOffset = 40;
        ok &= require(!g::validatePointStream(points), "point stream rejects incomplete final element");
    }
    return require(buffer.references == 1, "descriptor releases its retained buffer") && ok;
}
constexpr bool nodeRoundTrip() {
    g::NodeId node;
    return g::NodeId::encode(1, 9, (uint64_t(1) << 48) - 1, node) &&
        node.group() == 1 && node.level() == 9 && node.index() == (uint64_t(1) << 48) - 1;
}
constexpr bool branchingOrder() {
    g::LevelConfig config;
    return g::LevelConfig::fromBranching(3, 3, 3, 3, 5, config) &&
        config.logDimension[0] == 5 && (1u << config.logDimension[0]) == 32 &&
        config.logDimension[4] == 3 && config.initialNodeCapacity[2] == 2;
}
static_assert(nodeRoundTrip());
static_assert(branchingOrder());
static_assert(g::containsBytes(UINT64_MAX, UINT64_MAX - 3, 3));
static_assert(!g::containsBytes(UINT64_MAX, UINT64_MAX - 3, 4));
static_assert(g::containsElements(32, 4, 16, 2, 12));
static_assert(!g::containsElements(31, 4, 16, 2, 12));
static_assert(!g::containsElements(UINT64_MAX, 0, UINT64_MAX, UINT64_MAX, 1));
static_assert(g::containsElements(0, 0, 0, 0, 0));
static_assert(!std::is_copy_assignable_v<g::BindingSnapshot>);
static_assert(std::is_nothrow_copy_constructible_v<g::BindingSnapshot>);
static_assert(std::is_same_v<decltype(g::RecordingContext::commands), nvrhi::ICommandList*>);
}
int main() {
    bool ok = pointStreamDescriptors();
    uint64_t revision = 99;
    ok &= require(g::nextRevision(0, revision) && revision == 1, "first published revision is nonzero");
    ok &= require(!g::nextRevision(UINT64_MAX, revision) && revision == 0, "revision exhaustion cannot wrap");
    g::NodeId node;
    ok &= require(g::NodeId::encode(0, 2, 123, node), "encode valid node");
    ok &= require(!g::NodeId::encode(0, 2, uint64_t(1) << 48, node) && node.isNull(),
                  "reject reference truncation and clear output");
    ok &= require(!g::NodeId::encode(2, 0, 0, node) && node.isNull(), "reject unknown pool group");
    ok &= require(!g::NodeId::encode(0, 10, 0, node), "reject unsupported level");
    const g::Node emptyNode;
    ok &= require(emptyNode.parent.isNull() && emptyNode.children.isNull(), "default node references are null");
    g::LevelConfig config;
    ok &= require(g::LevelConfig::fromBranching(-2, 0, 3, 3, 5, config) &&
        config.logDimension[0] == 5 && config.logDimension[3] == 1 && config.logDimension[4] == 1,
        "root-to-leaf helper retains legacy nonpositive clamping");
    ok &= require(!g::LevelConfig::fromBranching(10, 10, 10, 10, 10, config) && config.levelCount == 0,
        "branching helper rejects overflow and clears output");
    ok &= require(!g::validateLevelConfig(config), "unconfigured hierarchy rejected");
    config.levelCount = 5;
    config.logDimension = {5, 3, 3, 3, 3};
    ok &= require(bool(g::validateLevelConfig(config)), "legacy five-level shape supported");
    config.levelCount = 4;
    config.logDimension = {10, 10, 10, 1};
    ok &= require(!g::validateLevelConfig(config), "reject signed index-space overflow");
    g::ChannelStorage storage;
    const g::AtlasFormat formats[] = {g::AtlasFormat::R8_UINT, g::AtlasFormat::RGBA8_UINT,
        g::AtlasFormat::R32_FLOAT, g::AtlasFormat::RGB32_FLOAT, g::AtlasFormat::RGBA32_FLOAT};
    const uint32_t strides[] = {1, 4, 4, 12, 16};
    for (unsigned i = 0; i < 5; ++i) {
        ok &= require(bool(g::describeChannel(formats[i], {3, 1, 1}, storage)), "supported raw channel format");
        ok &= require(storage.voxelStride == strides[i] && storage.logicalBytes == 3 * strides[i] &&
                      storage.allocatedBytes == ((3 * strides[i] + 3) & ~3u), "logical and padded raw bytes");
    }
    ok &= require(bool(g::describeChannel(g::AtlasFormat::R8_UINT, {}, storage)) && !storage.allocatedBytes,
                  "empty atlas has no live storage");
    ok &= require(!g::describeChannel(g::AtlasFormat::R8_UINT, {1, 0, 1}, storage) && !storage.logicalBytes,
                  "mixed zero dimensions rejected and output cleared");
    ok &= require(!g::describeChannel(g::AtlasFormat::R8_UINT, {UINT32_MAX, UINT32_MAX, UINT32_MAX}, storage),
                  "reject shape overflow without overflowing a three-axis product");
    ok &= require(bool(g::describeChannel(g::AtlasFormat::R8_UINT, {UINT32_MAX, 1, 1}, storage)) &&
                  storage.logicalBytes == UINT32_MAX && storage.allocatedBytes == g::c_AddressSpaceBytes,
                  "R8 logical length excludes trailing alignment byte");
    ok &= require(!g::describeChannel(g::AtlasFormat::RGBA32_FLOAT, {UINT32_MAX, 1, 1}, storage),
                  "reject channel byte offsets outside portable address space");
    g::ChannelDesc channel;
    channel.format = g::AtlasFormat::RGBA8_UINT;
    channel.voidValue = {0, 255, 128, 255};
    ok &= require(bool(g::validateChannelDesc(channel)), "integer boundary values use byte storage units");
    channel.voidValue.w = .5f;
    ok &= require(!g::validateChannelDesc(channel), "fractional integer boundary values rejected");
    channel.voidValue.w = 256;
    ok &= require(!g::validateChannelDesc(channel), "reject invalid integer boundary value");
    channel.format = g::AtlasFormat::R32_FLOAT;
    channel.voidValue.x = std::numeric_limits<float>::quiet_NaN();
    ok &= require(!g::validateChannelDesc(channel), "reject nonfinite boundary value");
    g::BufferView noBuffer;
    ok &= require(bool(g::validateRawView(noBuffer, 0, false)), "empty raw view allowed");
    ok &= require(!g::validateRawView(noBuffer, 4, false), "missing nonempty resource rejected");
    g::PointStream points;
    ok &= require(bool(g::validatePointStream(points)), "empty point stream allowed");
    points.count = 1;
    ok &= require(!g::validatePointStream(points), "nonempty point stream requires a resource");
    g::SnapshotBindings description;
    description.revision = {1, 2, 3};
    ok &= require(description.volume.hierarchy.y == UINT32_MAX, "default snapshot root is explicitly null");
    const g::BindingSnapshot snapshot(description);
    description.revision.topology = 99;
    ok &= require(snapshot.bindings().revision.topology == 2, "snapshot copies metadata instead of borrowing it");
    const g::BindingSnapshot copy(snapshot);
    ok &= require(copy.bindings().volume.hierarchy.y == UINT32_MAX, "copy preserves explicit empty root");
    if (ok) std::puts("GVDB NVRHI draft CPU contract PASS (no runtime/factory implementation claimed)");
    return ok ? 0 : 1;
}

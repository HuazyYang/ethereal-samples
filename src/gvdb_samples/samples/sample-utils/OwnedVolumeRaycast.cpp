// SPDX-License-Identifier: Apache-2.0
#include "OwnedVolumeRaycast.h"
#include "ComputeRuntime.h"
#include <cmath>
#include <stdexcept>
using namespace nvrhi;
namespace gvdb::sample {
namespace {
void check(FRESULT status, const char* text) {
    if (NVRHI_FAILED(status)) throw std::runtime_error(std::string(text)+": "+std::to_string(status));
}
void require(bool condition, const char* text) { if (!condition) throw std::invalid_argument(text); }
BufferRange range(const portable::BorrowedBufferBinding& view) { return {view.byteOffset,view.byteSize}; }
void raw(const portable::BorrowedBufferBinding& view, uint64_t minimum) {
    const portable::BufferView checked{view.buffer,view.byteOffset,view.byteSize};
    require(bool(portable::validateRawView(checked,minimum,false)),"Owned raycast raw buffer range/format");
}
}
slang::RaycastOutput raycastOwnedVolume(ethereal::slang::ComputeRuntime& runtime,
    portable::SnapshotHandle snapshot, uint32_t channel, slang::RaycastParameters p,
    const std::vector<slang::Float4>& transfer, bool surface) {
    require(snapshot && snapshot->getDevice()==runtime.device(),"Owned raycast snapshot device");
    require(p.image.x/8+uint32_t(p.image.x%8!=0)<=65535 &&
        p.image.y/8+uint32_t(p.image.y%8!=0)<=65535,"Owned raycast dispatch dimensions");
    require(transfer.size()<=portable::c_AddressSpaceBytes/sizeof(slang::Float4),
        "Owned raycast transfer address range");
    slang::validateRaycastParameters(p,transfer);
    const uint64_t pixels=uint64_t(p.image.x)*p.image.y;
    require(pixels<=portable::c_AddressSpaceBytes/sizeof(slang::Float4),"Owned raycast output address range");
    const auto desc=snapshot->getDesc();
    require(desc.levelCount && desc.levelCount<=portable::c_MaxLevels &&
        desc.volume.hierarchy.z==desc.levelCount,"Owned raycast level metadata");
    portable::ChannelSnapshotDesc selected;
    check(snapshot->getChannel(channel,selected),"Owned raycast channel");
    require(selected.desc.format==portable::AtlasFormat::R32_FLOAT,"Owned raycast requires R32_FLOAT");
    portable::ChannelStorage storage;
    const auto resolution=desc.volume.atlasResolutionAndApron;
    require(bool(portable::describeChannel(selected.desc.format,{resolution.x,resolution.y,resolution.z},storage)) &&
        storage.logicalBytes==selected.storage.logicalBytes && storage.allocatedBytes==selected.storage.allocatedBytes &&
        storage.voxelCount==selected.storage.voxelCount && selected.storage.voxelStride==4,"Owned raycast atlas extent");
    raw(selected.raw,storage.allocatedBytes);
    p.volume=desc.volume;p.image.z=uint32_t(transfer.size());
    slang::RaycastOutput result;result.width=p.image.x;result.height=p.image.y;
    if (!desc.activeLeafCount) {
        // No null topology bindings or shader reads for a configured empty owner.
        const std::vector<slang::Float4> colors(size_t(pixels),p.background);
        const std::vector<float> depths(size_t(pixels),INFINITY);
        result.color=runtime.buffer(colors.size()*sizeof(colors[0]),sizeof(colors[0]),colors.data());
        result.depth=runtime.buffer(depths.size()*sizeof(float),sizeof(float),depths.data());
        return result;
    }
    require(storage.voxelCount && desc.volume.hierarchy.x<desc.levelCount,"Owned raycast active hierarchy");
    portable::BorrowedBufferBinding nodes,children,levels;
    check(snapshot->getTopologyBuffer(portable::TopologyBuffer::Nodes,nodes),"Owned raycast nodes");
    check(snapshot->getTopologyBuffer(portable::TopologyBuffer::Children,children),"Owned raycast children");
    check(snapshot->getTopologyBuffer(portable::TopologyBuffer::Levels,levels),"Owned raycast levels");
    raw(nodes,sizeof(portable::Node));raw(children,0);
    require(levels.buffer && portable::containsBytes(levels.buffer->getDesc().byteSize,levels.byteOffset,levels.byteSize) &&
        levels.byteOffset%sizeof(slang::LevelInfo)==0 && levels.byteSize>=uint64_t(desc.levelCount)*sizeof(slang::LevelInfo) &&
        levels.buffer->getDesc().structStride==sizeof(slang::LevelInfo),"Owned raycast structured level range");
    auto transferBuffer=runtime.buffer(transfer.size()*sizeof(slang::Float4),sizeof(slang::Float4),transfer.data());
    result.color=runtime.buffer(size_t(pixels)*sizeof(slang::Float4),sizeof(slang::Float4));
    result.depth=runtime.buffer(size_t(pixels)*sizeof(float),sizeof(float));
    std::vector<BindingSetItem> bindings={
        BindingSetItem::RawBuffer_SRV(0,selected.raw.buffer,range(selected.raw)),
        BindingSetItem::RawBuffer_SRV(1,nodes.buffer,range(nodes)),
        BindingSetItem::StructuredBuffer_SRV(3,levels.buffer,Format::UNKNOWN,range(levels)),
        BindingSetItem::StructuredBuffer_SRV(4,transferBuffer),
        BindingSetItem::StructuredBuffer_UAV(0,result.color),BindingSetItem::StructuredBuffer_UAV(1,result.depth)};
    // A root-only hierarchy has no child pool; the runtime supplies the module's
    // unused reflected child global with valid dummy storage in that case.
    if(children.buffer)bindings.push_back(BindingSetItem::RawBuffer_SRV(2,children.buffer,range(children)));
    runtime.dispatch("GVDBRawRaycast",surface?"raycastSurface":"raycastVolume",&p,sizeof(p),bindings,
        p.image.x/8+uint32_t(p.image.x%8!=0),p.image.y/8+uint32_t(p.image.y%8!=0));
    return result;
}
} // namespace gvdb::sample

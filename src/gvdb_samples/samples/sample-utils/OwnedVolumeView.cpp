// SPDX-License-Identifier: Apache-2.0
#include "OwnedVolumeView.h"
#include "ComputeRuntime.h"
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace gvdb::sample {
namespace {
using namespace nvrhi;
void require(bool v,const char* message) {if(!v)throw std::invalid_argument(message);}
void check(FRESULT r,const char* message) {
    if(NVRHI_FAILED(r))throw std::runtime_error(std::string(message)+": "+std::to_string(r));
}
BufferRange range(const portable::BorrowedBufferBinding& view) {return {view.byteOffset,view.byteSize};}
void raw(const portable::BorrowedBufferBinding& view,uint64_t minimum) {
    require(bool(portable::validateRawView({view.buffer,view.byteOffset,view.byteSize},minimum,false)),
        "Owned view raw range");
}
struct alignas(16) Settings {
    slang::UInt4 rectangle,lineGrid;
    slang::Float4 section;
    slang::UInt4 flags;
};
static_assert(sizeof(Settings)==64);
slang::RaycastOutput dispatch(ethereal::slang::ComputeRuntime& runtime,portable::SnapshotHandle snapshot,
    uint32_t channel,slang::RaycastParameters p,const std::vector<slang::Float4>& transfer,
    OwnedVolumeViewMode mode,float sectionY,slang::UInt4 rectangle,const slang::RaycastOutput* existing) {
    require(snapshot&&snapshot->getDevice()==runtime.device(),"Owned view snapshot device");
    require(uint32_t(mode)<=uint32_t(OwnedVolumeViewMode::Section2D)&&std::isfinite(sectionY),"Owned view mode/section");
    require(p.image.x<=65535u*8&&p.image.y<=65535u*8,"Owned view dispatch dimensions");
    require(transfer.size()<=portable::c_AddressSpaceBytes/16,"Owned view transfer range");
    slang::validateRaycastParameters(p,transfer);
    const uint64_t pixels=uint64_t(p.image.x)*p.image.y;
    require(pixels<=portable::c_AddressSpaceBytes/16,"Owned view output range");
    require(rectangle.z&&rectangle.w&&rectangle.x<=p.image.x&&rectangle.y<=p.image.y&&
        rectangle.z<=p.image.x-rectangle.x&&rectangle.w<=p.image.y-rectangle.y,"Owned view rectangle");
    const auto info=snapshot->getDesc();
    require(info.levelCount&&info.levelCount<=portable::c_MaxLevels&&info.volume.hierarchy.z==info.levelCount,
        "Owned view level metadata");
    portable::ChannelSnapshotDesc selected;check(snapshot->getChannel(channel,selected),"Owned view channel");
    require(selected.desc.format==portable::AtlasFormat::R32_FLOAT,"Owned view requires R32_FLOAT");
    portable::ChannelStorage storage;const auto res=info.volume.atlasResolutionAndApron;
    require(bool(portable::describeChannel(selected.desc.format,{res.x,res.y,res.z},storage))&&
        storage.logicalBytes==selected.storage.logicalBytes&&storage.allocatedBytes==selected.storage.allocatedBytes&&
        storage.voxelCount==selected.storage.voxelCount&&selected.storage.voxelStride==4,"Owned view atlas extent");
    raw(selected.raw,storage.allocatedBytes);
    portable::BorrowedBufferBinding nodes,children,levels;
    if(info.activeLeafCount) {
        require(storage.voxelCount&&info.volume.hierarchy.x<info.levelCount,"Owned view active hierarchy");
        check(snapshot->getTopologyBuffer(portable::TopologyBuffer::Nodes,nodes),"Owned view nodes");
        check(snapshot->getTopologyBuffer(portable::TopologyBuffer::Children,children),"Owned view children");
        check(snapshot->getTopologyBuffer(portable::TopologyBuffer::Levels,levels),"Owned view levels");
        raw(nodes,sizeof(portable::Node));raw(children,0);
        require(levels.buffer&&levels.buffer->getDesc().structStride==sizeof(slang::LevelInfo)&&
            portable::containsBytes(levels.buffer->getDesc().byteSize,levels.byteOffset,levels.byteSize)&&
            levels.byteOffset%sizeof(slang::LevelInfo)==0&&levels.byteSize>=uint64_t(info.levelCount)*sizeof(slang::LevelInfo),
            "Owned view levels range");
    }
    const char* entry=mode==OwnedVolumeViewMode::Voxel?"renderVoxel":
        mode==OwnedVolumeViewMode::Section?"renderSection":"renderSection2D";
    runtime.requireViewKernel("OwnedVolumeView",entry);
    slang::RaycastOutput result;
    if(existing) {
        result=*existing;
        require(result.width==p.image.x&&result.height==p.image.y&&result.color&&result.depth&&
            result.color->getDesc().structStride==16&&result.depth->getDesc().structStride==4&&
            result.color->getDesc().canHaveUAVs&&result.depth->getDesc().canHaveUAVs&&
            result.color->getDesc().byteSize>=pixels*16&&result.depth->getDesc().byteSize>=pixels*4,
            "Owned view existing output");
    } else {
        result.width=p.image.x;result.height=p.image.y;
        result.color=runtime.buffer(size_t(pixels)*16,16);result.depth=runtime.buffer(size_t(pixels)*4,4);
    }
    auto tf=runtime.buffer(transfer.size()*16,16,transfer.data());
    Settings settings{rectangle,{}, {0,sectionY,0,0},{info.activeLeafCount?1u:0u,0,0,0}};
    BufferDesc desc;desc.byteSize=256;desc.isConstantBuffer=true;
    desc.initialState=ResourceStates::ConstantBuffer;desc.keepInitialState=true;
    BufferHandle constants;check(runtime.device()->createBuffer(desc,&constants),"Owned view constants");
    std::array<uint8_t,256> bytes{};std::memcpy(bytes.data(),&settings,sizeof(settings));
    runtime.upload(constants,bytes.data(),bytes.size());
    std::vector<BindingSetItem> bindings={BindingSetItem::ConstantBuffer(1,constants),
        BindingSetItem::StructuredBuffer_SRV(4,tf),BindingSetItem::StructuredBuffer_UAV(0,result.color),
        BindingSetItem::StructuredBuffer_UAV(1,result.depth)};
    if(selected.raw.buffer)bindings.push_back(BindingSetItem::RawBuffer_SRV(0,selected.raw.buffer,range(selected.raw)));
    if(nodes.buffer)bindings.push_back(BindingSetItem::RawBuffer_SRV(1,nodes.buffer,range(nodes)));
    if(children.buffer)bindings.push_back(BindingSetItem::RawBuffer_SRV(2,children.buffer,range(children)));
    if(levels.buffer)bindings.push_back(BindingSetItem::StructuredBuffer_SRV(3,levels.buffer,Format::UNKNOWN,range(levels)));
    p.volume=info.volume;p.image.z=uint32_t(transfer.size());
    runtime.dispatch("OwnedVolumeView",entry,&p,sizeof(p),bindings,(rectangle.z+7)/8,(rectangle.w+7)/8);
    return result;
}
} // namespace
slang::RaycastOutput renderOwnedVolumeView(ethereal::slang::ComputeRuntime& runtime,
    portable::SnapshotHandle snapshot,uint32_t channel,slang::RaycastParameters p,
    const std::vector<slang::Float4>& transfer,OwnedVolumeViewMode mode,float sectionY) {
    return dispatch(runtime,std::move(snapshot),channel,p,transfer,mode,sectionY,{0,0,p.image.x,p.image.y},nullptr);
}
void drawOwnedVolumeSection(ethereal::slang::ComputeRuntime& runtime,portable::SnapshotHandle snapshot,
    uint32_t channel,slang::RaycastParameters p,const std::vector<slang::Float4>& transfer,
    float sectionY,slang::UInt4 rectangle,const slang::RaycastOutput& output) {
    dispatch(runtime,std::move(snapshot),channel,p,transfer,OwnedVolumeViewMode::Section2D,sectionY,rectangle,&output);
}
} // namespace gvdb::sample

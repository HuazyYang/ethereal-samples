// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiPointApi.h"
#include "GVDBNvrhiPointApiInternal.h"
#include "GVDBNvrhiApiInternal.h"
#include "GVDBNvrhiAdoptionInternal.h"
#include "GVDBPointStreamConversion.h"
#include <nvrhi/shader-program.h>
#include <nvrhi/core/foundation.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>
namespace gvdb::portable {
namespace {
using namespace nvrhi;
struct Failure { FRESULT result; };
void require(bool value,FRESULT result=FE_INVALID_ARGS) { if(!value)throw Failure{result}; }
void check(FRESULT result) { require(NVRHI_SUCCEEDED(result),result); }
template<class F> FRESULT guarded(F&& function) noexcept {
    try { return function(); }
    catch(const Failure& failure) { return failure.result; }
    catch(const std::bad_alloc&) { return FE_OUT_OF_MEMORY; }
    catch(const std::length_error&) { return FE_OUT_OF_MEMORY; }
    catch(...) { return FE_GENERIC_ERROR; }
}
template<class T,class I> AutoPtr<T> implementation(I* object) {
    require(object!=nullptr);AutoPtr<T> result;
    require(NVRHI_SUCCEEDED(object->QueryInterface(uuid_of<T>(),reinterpret_cast<void**>(result.GetAddressOf())))&&result.Get());
    return result;
}
bool finite(Float4 value) { return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z)&&std::isfinite(value.w); }
uint64_t add(uint64_t a,uint64_t b,FRESULT code=FE_OUT_OF_MEMORY) { require(b<=UINT64_MAX-a,code);return a+b; }
uint64_t mul(uint64_t a,uint64_t b) { require(!a || b<=UINT64_MAX/a,FE_UNSUPPORTED);return a*b; }
uint64_t padded(uint64_t value,uint64_t alignment=4) { return add(value,alignment-1)&~(alignment-1); }
uint64_t sourceBytes(const HostPointAttributeDesc& attribute,uint32_t index) {
    const auto& stream=attribute.stream;
    require(finite(attribute.transform.minimum)&&finite(attribute.transform.delta)&&finite(attribute.transform.scale)&&finite(attribute.transform.translation));
    const auto width=componentBytes(stream.component);
    require(width!=0,FE_UNSUPPORTED);
    require(index==2 ? stream.components==4 : (stream.components==3 || stream.components==4));
    require(stream.count<=UINT32_MAX && stream.byteOffset<=UINT32_MAX && stream.byteStride<=UINT32_MAX && stream.byteSize<=SIZE_MAX,FE_UNSUPPORTED);
    require(containsElements(stream.byteSize,stream.byteOffset,stream.byteStride,stream.count,uint64_t(width)*stream.components));
    require(containsElements(c_AddressSpaceBytes,stream.byteOffset,stream.byteStride,stream.count,uint64_t(width)*stream.components),FE_UNSUPPORTED);
    if(!stream.count)return 0;
    require(stream.data!=nullptr);
    return stream.byteOffset+(stream.count-1)*stream.byteStride+uint64_t(width)*stream.components;
}
struct AllocationBudget {
    IDevice* device;
    const PointPreparationBudget& limits;
    uint64_t bytes=0;
    uint32_t count=0;
    BufferHandle create(uint64_t size,bool raw,bool constant=false,bool readback=false) {
        require(size && size<=c_AddressSpaceBytes,FE_UNSUPPORTED);
        require(count<limits.maxBuffers && size<=limits.maxAllocatedBytes-bytes,FE_OUT_OF_MEMORY);
        BufferDesc desc;desc.byteSize=size;desc.canHaveRawViews=raw;desc.canHaveUAVs=raw&&!readback;
        desc.isConstantBuffer=constant;desc.cpuAccess=readback?CpuAccessMode::Read:CpuAccessMode::None;
        desc.initialState=constant?ResourceStates::ConstantBuffer:ResourceStates::CopyDest;desc.keepInitialState=true;
        desc.debugName="GVDB owned point conversion";
        BufferHandle result;check(device->createBuffer(desc,&result));require(bool(result),FE_OUT_OF_MEMORY);
        bytes+=size;++count;return result;
    }
};
struct Program {
    ShaderProgramHandle artifact;
    BindingLayoutHandle layout;
    ComputePipelineHandle pipeline;
};
Program program(IDevice* device,const void* data,size_t bytes) {
    Program result;check(createShaderProgram(device,data,bytes,&result.artifact));
    uint32_t entry;check(result.artifact->findEntryPoint("convertPointStream",ShaderType::Compute,&entry));
    ShaderEntryPointDesc entryDesc;check(result.artifact->getEntryPointDesc(entry,entryDesc));
    require(entryDesc.threadGroupSize[0]==256 && entryDesc.threadGroupSize[1]==1 && entryDesc.threadGroupSize[2]==1 && entryDesc.entryParamsSize==0,FE_UNSUPPORTED);
    BindingLayoutDesc layout;check(result.artifact->getBindingLayoutDesc(entry,0,layout));
    require(layout.bindings.size()==3,FE_UNSUPPORTED);
    for(auto type:{ResourceType::ConstantBuffer,ResourceType::RawBuffer_SRV,ResourceType::RawBuffer_UAV})
        require(std::count_if(layout.bindings.begin(),layout.bindings.end(),[&](const auto& item){return item.type==type&&item.slot==0;})==1,FE_UNSUPPORTED);
    uint32_t globals=0,members=0,mask=0,constant=UINT32_MAX;
    for(uint32_t i=0;i<result.artifact->getParameterCount(entry);++i) {
        ShaderParameterDesc item;check(result.artifact->getParameterDesc(entry,i,item));
        require(item.registerSpace==0 && item.count==1,FE_UNSUPPORTED);
        if(item.scope==ShaderParameterScope::Global) {
            ++globals;require(item.parentParameter==UINT32_MAX && item.registerIndex==0,FE_UNSUPPORTED);
            if(item.resourceType==ResourceType::ConstantBuffer) {require(item.elementStride==sizeof(PointStreamConversionParameters),FE_UNSUPPORTED);constant=i;}
            else require((item.resourceType==ResourceType::RawBuffer_SRV||item.resourceType==ResourceType::RawBuffer_UAV)&&item.elementStride==1,FE_UNSUPPORTED);
        } else {
            require(item.scope==ShaderParameterScope::ConstantBuffer && item.resourceType==ResourceType::None && item.size==16 && item.elementStride==16
                && item.offset<sizeof(PointStreamConversionParameters) && item.offset%16==0,FE_UNSUPPORTED);
            const auto bit=1u<<(item.offset/16);require(!(mask&bit),FE_UNSUPPORTED);mask|=bit;++members;
        }
    }
    require(globals==3&&members==7&&mask==127&&constant!=UINT32_MAX,FE_UNSUPPORTED);
    for(uint32_t i=0;i<result.artifact->getParameterCount(entry);++i) {
        ShaderParameterDesc item;check(result.artifact->getParameterDesc(entry,i,item));
        if(item.scope==ShaderParameterScope::ConstantBuffer)require(item.parentParameter==constant,FE_UNSUPPORTED);
    }
    check(device->createBindingLayout(layout,&result.layout));ShaderHandle shader;check(result.artifact->createShader(entry,&shader));
    check(device->createComputePipeline(ComputePipelineDesc().setComputeShader(shader).addBindingLayout(result.layout),&result.pipeline));return result;
}
struct PointOwnerIdentity {};
struct AttributeData {
    BufferHandle buffer;
    std::vector<uint8_t> bytes;
    PointComponent component=PointComponent::Float32;
    uint32_t components=3;
};
NVRHI_CLASS_CLSID(PointSet,"1baab8b9-3e49-4c1d-93c1-90611242c257")
class PointSet final:public ObjectImpl<IGVDBPointSet> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(PointSet)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(PointSet)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBPointSet)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(PointSet)
    NVRHI_END_INTERFACE_TABLE()
    DeviceHandle device;
    PointSetInfo info;
    std::array<AttributeData,3> attributes;
    PointSet(IDevice* d,PointSetInfo i,std::array<AttributeData,3> values) noexcept:device(d),info(i),attributes(std::move(values)) {}
    IDevice* getDevice() const noexcept override {return device.Get();}
    PointSetInfo getDesc() const noexcept override {return info;}
    FRESULT getStream(PointAttribute type,BorrowedPointStream& output) const noexcept override {
        output={};const auto index=uint32_t(type);if(index>=3)return FE_INVALID_ARGS;
        if(!(info.attributeMask&(1u<<index)))return FE_NOT_FOUND;
        const auto& value=attributes[index];
        output={value.buffer.Get(),value.bytes.size(),0,uint64_t(value.components)*componentBytes(value.component),info.count,value.component,value.components};
        return FS_OK;
    }
    FRESULT getData(PointAttribute type,BorrowedBytes& output) const noexcept override {
        output={};const auto index=uint32_t(type);if(index>=3)return FE_INVALID_ARGS;
        if(!(info.attributeMask&(1u<<index)))return FE_NOT_FOUND;
        const auto& value=attributes[index].bytes;output={value.empty()?nullptr:value.data(),value.size()};return FS_OK;
    }
};
struct PreparedAttribute {
    BufferHandle source,destination,constants,staging;
    BindingSetHandle bindings;
    std::vector<uint8_t> sourceBytes;
    PointStreamConversionParameters parameters{};
    uint64_t logicalBytes=0;
    uint32_t components=3,groupsX=0,groupsY=0;
    PointComponent component=PointComponent::Float32;
};
NVRHI_CLASS_CLSID(PointUpload,"ef754abc-7d56-4ca6-afd2-0ed2c8372c27")
class PointUpload final:public ObjectImpl<IGVDBPointUploadTicket> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(PointUpload)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(PointUpload)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBPointUploadTicket)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(PointUpload)
    NVRHI_END_INTERFACE_TABLE()
    DeviceHandle device;
    std::shared_ptr<PointOwnerIdentity> owner;
    Program program;
    std::array<PreparedAttribute,3> attributes;
    uint64_t count=0;
    uint32_t mask=1;
    bool recorded=false,completed=false;
    PointUpload(IDevice* d,std::shared_ptr<PointOwnerIdentity> identity,Program shader):device(d),owner(std::move(identity)),program(std::move(shader)) {}
    IDevice* getDevice() const noexcept override {return device.Get();}
    FRESULT finishAfterCallerCompletion(IGVDBPointSet** output) noexcept override {
        if(!output)return FE_INVALID_ARGS;*output=nullptr;
        return guarded([&] {
            require(recorded&&!completed);
            std::array<AttributeData,3> data;
            for(uint32_t index=0;index<3;++index)if(mask&(1u<<index)) {
                const auto& source=attributes[index];auto& target=data[index];
                target.buffer=source.destination;target.component=source.component;target.components=source.components;
                if(source.logicalBytes) {
                    target.bytes.resize(size_t(source.logicalBytes));
                    const void* mapped=device->mapBuffer(source.staging,CpuAccessMode::Read);require(mapped,FE_GENERIC_ERROR);
                    std::memcpy(target.bytes.data(),mapped,target.bytes.size());device->unmapBuffer(source.staging);
                }
            }
            PointSetInfo info;info.count=count;info.attributeMask=mask;info.empty=count==0;
            for(uint64_t point=0;point<count;++point) {
                Float4 position{};std::memcpy(&position,data[0].bytes.data()+point*data[0].components*4,data[0].components*4);
                require(std::isfinite(position.x)&&std::isfinite(position.y)&&std::isfinite(position.z));
                position.w=0;
                if(!point)info.boundsMin=info.boundsMax=position;
                else {
                    info.boundsMin={std::min(info.boundsMin.x,position.x),std::min(info.boundsMin.y,position.y),std::min(info.boundsMin.z,position.z),0};
                    info.boundsMax={std::max(info.boundsMax.x,position.x),std::max(info.boundsMax.y,position.y),std::max(info.boundsMax.z,position.z),0};
                }
            }
            auto result=MAKE_RC_OBJ_PTR(PointSet,device.Get(),info,std::move(data));completed=true;*output=result.Detach();return FS_OK;
        });
    }
};
class PointOps final:public ObjectImpl<IGVDBPointOps> {
    VolumeHandle m_volume;
    NvrhiVolume* m_core;
    std::shared_ptr<PointOwnerIdentity> m_owner;
    Program m_program;
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(PointOps)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBPointOps)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    PointOps(VolumeHandle volume,NvrhiVolume* core,Program shader):m_volume(std::move(volume)),m_core(core),m_owner(std::make_shared<PointOwnerIdentity>()),m_program(std::move(shader)) {}
    IGVDBVolume* getVolume() const noexcept override {return m_volume.Get();}
    FRESULT preparePoints(const PointSetDesc& desc,const PointPreparationBudget& budget,IGVDBPointUploadTicket** output) noexcept override {
        if(!output)return FE_INVALID_ARGS;*output=nullptr;
        return guarded([&] {
            require(desc.structSize==sizeof(PointSetDesc)&&!desc.reserved&&!budget.reserved);
            const HostPointAttributeDesc* sources[]={&desc.positions,&desc.velocities,&desc.colors};
            const uint64_t count=desc.positions.stream.count;
            std::array<uint64_t,3> copied{};uint64_t inputBytes=0;
            for(uint32_t i=0;i<3;++i) {
                require(!i || !sources[i]->stream.count || sources[i]->stream.count==count);
                copied[i]=sourceBytes(*sources[i],i);inputBytes=add(inputBytes,copied[i]);
            }
            require(inputBytes<=budget.maxInputBytes,FE_OUT_OF_MEMORY);
            auto result=MAKE_RC_OBJ_PTR(PointUpload,m_volume->getDevice(),m_owner,m_program);result->count=count;
            AllocationBudget allocator{m_volume->getDevice(),budget};
            for(uint32_t index=0;index<3;++index) {
                const auto& source=*sources[index];auto& target=result->attributes[index];
                target.components=source.stream.components;target.component=index==2?PointComponent::UInt8:PointComponent::Float32;
                if(!source.stream.count)continue;
                result->mask|=1u<<index;
                target.logicalBytes=mul(mul(count,target.components),componentBytes(target.component));
                target.source=allocator.create(padded(copied[index]),true);
                target.destination=allocator.create(padded(target.logicalBytes),true);
                target.constants=allocator.create(256,false,true);
                target.staging=allocator.create(padded(target.logicalBytes),false,false,true);
                target.sourceBytes.resize(size_t(padded(copied[index])),0);
                std::memcpy(target.sourceBytes.data(),source.stream.data,size_t(copied[index]));
                PointStream from{target.source,source.stream.byteOffset,source.stream.byteStride,count,source.stream.component,source.stream.components};
                PointStream to{target.destination,0,uint64_t(target.components)*componentBytes(target.component),count,target.component,target.components};
                PointStreamConversion conversion;
                check(preparePointStreamConversion(from,to,{source.transform.minimum,source.transform.delta,source.transform.scale,source.transform.translation},conversion));
                target.parameters=conversion.parameters;target.groupsX=conversion.groupsX;target.groupsY=conversion.groupsY;
                BindingSetDesc bindings;bindings.addItem(BindingSetItem::ConstantBuffer(0,target.constants));
                bindings.addItem(BindingSetItem::RawBuffer_SRV(0,target.source));bindings.addItem(BindingSetItem::RawBuffer_UAV(0,target.destination));
                check(m_volume->getDevice()->createBindingSet(bindings,m_program.layout,&target.bindings));
            }
            *output=result.Detach();return FS_OK;
        });
    }
    FRESULT recordPoints(RecordingContext context,IGVDBPointUploadTicket* ticket) noexcept override {
        return guarded([&] {
            const auto value=implementation<PointUpload>(ticket);
            require(value->owner==m_owner&&!value->recorded&&!value->completed);
            require(context.device==m_volume->getDevice()&&context.commands&&context.commands->getDevice()==context.device);
            require(context.commands->getDesc().queueType!=CommandQueue::Copy,FE_UNSUPPORTED);
            value->recorded=true;
            for(const auto& source:value->attributes)if(source.logicalBytes) {
                context.commands->writeBuffer(source.source,source.sourceBytes.data(),source.sourceBytes.size());
                std::array<uint8_t,256> constants{};
                std::memcpy(constants.data(),&source.parameters,sizeof(source.parameters));
                context.commands->writeBuffer(source.constants,constants.data(),constants.size());
                context.commands->clearBufferUInt(source.destination,0);
                ComputeState state;state.setPipeline(value->program.pipeline).addBindingSet(source.bindings);
                context.commands->setComputeState(state);context.commands->dispatch(source.groupsX,source.groupsY,1);
                context.commands->copyBuffer(source.staging,0,source.destination,0,source.destination->getDesc().byteSize);
            }
            return FS_OK;
        });
    }
    FRESULT prepareTopology(IGVDBPointSet*,const PointTopologyDesc&,const PlacementDesc&,const PointPreparationBudget&,IGVDBGenerationTicket**) noexcept override;
};
FRESULT PointOps::prepareTopology(IGVDBPointSet* points,const PointTopologyDesc& desc,
    const PlacementDesc& placement,const PointPreparationBudget& budget,IGVDBGenerationTicket** output) noexcept {
    if(!output)return FE_INVALID_ARGS;*output=nullptr;
    return guarded([&] {
        require(desc.structSize==sizeof(PointTopologyDesc)&&!desc.reserved&&!desc.reserved2&&!desc.reserved3&&!budget.reserved);
        require(desc.mode==PointTopologyMode::FullRebuild||desc.mode==PointTopologyMode::Accumulate);
        require(std::isfinite(desc.radius)&&desc.radius>=0&&desc.preserveMetadata<=1&&placement.preserve<=1);
        require(desc.minDepth<m_core->host().configuration().levelCount);
        const bool accumulating=desc.mode==PointTopologyMode::Accumulate;
        require(!accumulating||(desc.preserveMetadata&&placement.preserve));
        const auto data=implementation<PointSet>(points);
        require(data->device.Get()==m_volume->getDevice());
        const uint32_t width=m_core->host().range(0);
        require(width!=0);
        struct Support {Int3 center,lower,upper;bool expanded=false;};
        std::vector<Support> ranges;
        require(data->info.count<=ranges.max_size()&&data->info.count<=budget.maxActivationCandidates,FE_OUT_OF_MEMORY);
        ranges.reserve(size_t(data->info.count));
        uint64_t candidates=0;
        auto account=[&](uint64_t count) {
            require(count<=budget.maxActivationCandidates-candidates,FE_OUT_OF_MEMORY);candidates+=count;
        };
        auto integer=[](double value) {
            require(std::isfinite(value)&&value>=INT32_MIN&&value<=INT32_MAX,FE_UNSUPPORTED);
            return int32_t(value);
        };
        auto floorBrick=[&](int32_t value) {
            int64_t quotient=int64_t(value)/width;
            if(value<0&&int64_t(value)%width)--quotient;
            const int64_t origin=quotient*width;
            require(origin>=INT32_MIN&&origin<=INT32_MAX,FE_UNSUPPORTED);
            return int32_t(origin);
        };
        for(uint64_t i=0;i<data->info.count;++i) {
            Float4 position{};
            std::memcpy(&position,data->attributes[0].bytes.data()+i*data->attributes[0].components*4,data->attributes[0].components*4);
            Support support;
            support.center={floorBrick(integer(std::floor(double(position.x)))),floorBrick(integer(std::floor(double(position.y)))),floorBrick(integer(std::floor(double(position.z))))};
            account(1); // The actual containing brick also covers negative fractional positions.
            support.expanded=accumulating||desc.radius>=1;
            if(support.expanded) {
                const float upperRadius=accumulating?desc.radius:2.f*desc.radius;
                const float values[]={position.x,position.y,position.z};
                int32_t lower[3],upper[3];uint64_t count=1;
                for(uint32_t axis=0;axis<3;++axis) {
                    const float truncated=float(integer(double(values[axis])));
                    const float lo=truncated-desc.radius,hi=truncated+upperRadius;
                    lower[axis]=floorBrick(integer(double(lo)));
                    upper[axis]=floorBrick(integer(double(hi)));
                    require(lower[axis]<=upper[axis]);
                    const auto cells=uint64_t((int64_t(upper[axis])-lower[axis])/width)+1;
                    require(cells<=budget.maxActivationCandidates,FE_OUT_OF_MEMORY);
                    require(!count||cells<=UINT64_MAX/count,FE_OUT_OF_MEMORY);count*=cells;
                }
                support.lower={lower[0],lower[1],lower[2]};support.upper={upper[0],upper[1],upper[2]};account(count);
            }
            ranges.push_back(support);
        }
        std::unique_ptr<detail::AccumulationCandidate> accumulation;
        std::unique_ptr<HostVolume> rebuild;
        if(accumulating)check(detail::VolumeAdoptionBuilder::beginAccumulation(*m_core,accumulation));
        else check(detail::VolumeAdoptionBuilder::beginRebuild(*m_core,rebuild));
        HostVolume& candidate=accumulating?accumulation->host():*rebuild;
        auto activate=[&](Int3 origin) {
            NodeId leaf;check(candidate.activateAtLevel(0,origin,leaf));check(candidate.setActive(leaf,true));
        };
        // Preserve the legacy centers-before-support phase ordering, then x/y/z support order.
        for(const auto& support:ranges)activate(support.center);
        for(const auto& support:ranges)if(support.expanded)
            for(int64_t x=support.lower.x;x<=support.upper.x;x+=width)
                for(int64_t y=support.lower.y;y<=support.upper.y;y+=width)
                    for(int64_t z=support.lower.z;z<=support.upper.z;z+=width)activate({int32_t(x),int32_t(y),int32_t(z)});
        if(!candidate.root().isNull()&&candidate.root().level()<desc.minDepth) {
            const auto origin=candidate.node(candidate.root())->position;
            NodeId promoted;check(candidate.activateAtLevel(desc.minDepth,origin,promoted));
        }
        check(candidate.finishTopology());
        GenerationRequest request;
        request.placement={placement.minimumBrickCount,placement.apron,placement.preserve!=0};
        request.budget={budget.maxAllocatedBytes,budget.maxBuffers};
        std::unique_ptr<PreparedGeneration> prepared;
        if(accumulating)check(detail::VolumeAdoptionBuilder::prepareAccumulation(*m_core,*accumulation,request,prepared));
        else check(detail::VolumeAdoptionBuilder::prepareTopology(*m_core,candidate,request,desc.preserveMetadata!=0,prepared));
        return detail::wrapGeneration(m_volume->getDevice(),std::move(prepared),output);
    });
}
} // namespace
namespace detail {
nvrhi::FRESULT resolvePointSet(IGVDBPointSet* input,nvrhi::IDevice* device,ResolvedPointSet& output) noexcept {
    return guarded([&] {
        require(device!=nullptr);
        const auto point=implementation<PointSet>(input);
        require(point->device.Get()==device);
        ResolvedPointSet result;result.retained=point.Get();result.info=point->info;
        check(point->getData(PointAttribute::Position,result.positions));
        check(point->getStream(PointAttribute::Position,result.positionStream));
        if(result.info.attributeMask&(1u<<uint32_t(PointAttribute::Color))) {
            check(point->getData(PointAttribute::Color,result.colors));
            check(point->getStream(PointAttribute::Color,result.colorStream));
        }
        output=std::move(result);return FS_OK;
    });
}
} // namespace detail
nvrhi::FRESULT gvdbCreateNvrhiPointOps(uint32_t apiVersion,const PointOpsCreateDesc* desc,IGVDBPointOps** output) noexcept {
    if(!output)return FE_INVALID_ARGS;*output=nullptr;
    return guarded([&] {
        require(apiVersion==c_GVDBNvrhiApiVersion,FE_UNSUPPORTED);
        require(desc&&desc->structSize==sizeof(PointOpsCreateDesc)&&!desc->reserved&&desc->volume&&desc->conversionArtifact&&desc->conversionArtifactBytes&&desc->conversionArtifactBytes<=SIZE_MAX);
        VolumeHandle volume=desc->volume;NvrhiVolume* core=nullptr;check(detail::resolveVolume(volume,&core));
        auto shader=program(volume->getDevice(),desc->conversionArtifact,size_t(desc->conversionArtifactBytes));
        auto result=MAKE_RC_OBJ_PTR(PointOps,std::move(volume),core,std::move(shader));*output=result.Detach();return FS_OK;
    });
}
} // namespace gvdb::portable

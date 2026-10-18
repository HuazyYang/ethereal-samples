// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiResample.h"
#include "GVDBNvrhiMutationInternal.h"
#include <nvrhi/shader-program.h>
#include <algorithm>
#include <cmath>
#include <new>
#include <stdexcept>
namespace gvdb::portable {
namespace {
using namespace nvrhi;
struct Failure { FRESULT result; };
void require(bool value,FRESULT result=FE_INVALID_ARGS) { if(!value)throw Failure{result}; }
void check(FRESULT result) { require(NVRHI_SUCCEEDED(result),result); }
template<class F> FRESULT guarded(F&& function) noexcept {
    try {function();return FS_OK;}
    catch(const Failure& failure){return failure.result;}
    catch(const std::bad_alloc&){return FE_OUT_OF_MEMORY;}
    catch(const std::length_error&){return FE_OUT_OF_MEMORY;}
    catch(...){return FE_GENERIC_ERROR;}
}
bool finite(Float4 value) {
    return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z)&&std::isfinite(value.w);
}
struct alignas(16) Parameters {
    slang::UInt4 resolutionAndApron,brickCountAndWidth,sourceResolutionAndWork;
    Float4 rowX,rowY,rowZ,remap;
    slang::UInt4 options;
    Float4 background;
};
static_assert(sizeof(Parameters)==144);
void validateArtifact(IShaderProgram* program,uint32_t entry,const BindingLayoutDesc& layout) {
    ShaderEntryPointDesc desc;check(program->getEntryPointDesc(entry,desc));
    require(desc.entryParamsSize==0 && desc.threadGroupSize[0]==256 && desc.threadGroupSize[1]==1 && desc.threadGroupSize[2]==1,FE_UNSUPPORTED);
    const ResourceType types[]={ResourceType::ConstantBuffer,ResourceType::RawBuffer_SRV,
        ResourceType::RawBuffer_SRV,ResourceType::StructuredBuffer_SRV,ResourceType::RawBuffer_UAV};
    const uint32_t slots[]={0,0,1,2,0};
    const uint32_t strides[]={sizeof(Parameters),1,1,sizeof(slang::AtlasNode),1};
    require(layout.bindings.size()==5,FE_UNSUPPORTED);
    for(uint32_t i=0;i<5;++i)require(std::count_if(layout.bindings.begin(),layout.bindings.end(),
        [&](const auto& binding){return binding.type==types[i]&&binding.slot==slots[i];})==1,FE_UNSUPPORTED);
    uint32_t globals=0,resources=0,members=0,mask=0,constant=UINT32_MAX;
    for(uint32_t i=0;i<program->getParameterCount(entry);++i) {
        ShaderParameterDesc item;check(program->getParameterDesc(entry,i,item));
        require(item.registerSpace==0&&item.count==1,FE_UNSUPPORTED);
        if(item.scope==ShaderParameterScope::Global) {
            ++globals;require(item.parentParameter==UINT32_MAX,FE_UNSUPPORTED);
            uint32_t found=5;
            for(uint32_t j=0;j<5;++j)if(item.resourceType==types[j]&&item.registerIndex==slots[j])found=j;
            require(found<5,FE_UNSUPPORTED);
            require(!(resources&(1u<<found))&&item.elementStride==strides[found],FE_UNSUPPORTED);
            resources|=1u<<found;
            if(item.resourceType==ResourceType::ConstantBuffer)constant=i;
        } else {
            require(item.scope==ShaderParameterScope::ConstantBuffer&&item.resourceType==ResourceType::None&&
                item.offset<sizeof(Parameters)&&item.offset%16==0&&item.size==16&&item.elementStride==16,FE_UNSUPPORTED);
            const uint32_t bit=1u<<(item.offset/16);require(!(mask&bit),FE_UNSUPPORTED);mask|=bit;++members;
        }
    }
    require(globals==5&&resources==31&&members==9&&mask==511&&constant!=UINT32_MAX,FE_UNSUPPORTED);
    for(uint32_t i=0;i<program->getParameterCount(entry);++i) {
        ShaderParameterDesc item;check(program->getParameterDesc(entry,i,item));
        if(item.scope==ShaderParameterScope::ConstantBuffer)require(item.parentParameter==constant,FE_UNSUPPORTED);
    }
}
}
struct NvrhiResample::Impl {
    DeviceHandle device;
    NvrhiVolume* owner=nullptr;
    ShaderProgramHandle program;
    BindingLayoutHandle layout;
    ComputePipelineHandle pipeline;
};
NvrhiResample::NvrhiResample(std::unique_ptr<Impl> impl) noexcept:m_impl(std::move(impl)){}
NvrhiResample::~NvrhiResample()=default;
FRESULT NvrhiResample::create(const ResampleOperatorDesc& desc,std::unique_ptr<NvrhiResample>& output) noexcept {
    return guarded([&] {
        require(desc.volume&&desc.artifact&&desc.artifactBytes);
        auto impl=std::make_unique<Impl>();impl->owner=desc.volume;impl->device=detail::VolumeMutationBuilder::device(*desc.volume);
        check(createShaderProgram(impl->device,desc.artifact,desc.artifactBytes,&impl->program));
        uint32_t entry;check(impl->program->findEntryPoint("resampleOwnedAtlas",ShaderType::Compute,&entry));
        BindingLayoutDesc layout;check(impl->program->getBindingLayoutDesc(entry,0,layout));validateArtifact(impl->program,entry,layout);
        check(impl->device->createBindingLayout(layout,&impl->layout));ShaderHandle shader;check(impl->program->createShader(entry,&shader));
        check(impl->device->createComputePipeline(ComputePipelineDesc().setComputeShader(shader).addBindingLayout(impl->layout),&impl->pipeline));
        output=std::unique_ptr<NvrhiResample>(new NvrhiResample(std::move(impl)));
    });
}
FRESULT NvrhiResample::prepare(const ResampleRequest& request,const PreparationBudget& budget,
    std::unique_ptr<PreparedGeneration>& output) noexcept {
    return guarded([&] {
        require(request.channel<c_MaxChannels);
        const auto& source=request.source;
        const auto extent=source.resolution;
        require(source.data&&extent.x&&extent.y&&extent.z);
        const uint64_t plane=uint64_t(extent.x)*extent.y;
        require(plane<=c_AddressSpaceBytes/sizeof(float)&&extent.z<=c_AddressSpaceBytes/sizeof(float)/plane,FE_UNSUPPORTED);
        const uint64_t sourceBytes=plane*extent.z*sizeof(float);
        require(source.byteSize>=sourceBytes&&sourceBytes<=SIZE_MAX);
        for(const auto& row:request.transformRows)require(finite(row));
        require(finite(request.remap)&&std::isfinite(request.background));
        const float denominator=request.remap.y-request.remap.x;
        require(std::isfinite(denominator)&&denominator!=0&&std::isfinite(request.remap.w-request.remap.z));
        auto& state=*m_impl;const auto snapshot=state.owner->snapshot();require(bool(snapshot),FE_NOT_ALIVE_OBJECT);
        const auto& current=snapshot->bindings();const uint32_t mask=uint32_t(1)<<request.channel;
        require((current.channelMask&mask)!=0,FE_NOT_FOUND);
        require(current.channels[request.channel].desc.format==AtlasFormat::R32_FLOAT,FE_UNSUPPORTED);
        std::unique_ptr<detail::VolumeMutationBuilder> builder;
        check(detail::VolumeMutationBuilder::begin(*state.owner,mask,budget,builder));
        if(!builder->hasWork()){check(builder->finish(output));return;}
        const auto& destination=builder->destinationBindings();const auto& selected=destination.channels[request.channel];
        Parameters p{};p.resolutionAndApron=destination.volume.atlasResolutionAndApron;
        p.brickCountAndWidth=destination.volume.atlasBrickCountAndWidth;
        p.sourceResolutionAndWork={extent.x,extent.y,extent.z,selected.storage.voxelCount};
        p.rowX=request.transformRows[0];p.rowY=request.transformRows[1];p.rowZ=request.transformRows[2];p.remap=request.remap;
        p.options.x=request.preserveOutside?1u:0u;p.background.x=request.background;
        BufferHandle dense,constants;
        check(builder->upload(source.data,sourceBytes,0,true,false,dense));
        check(builder->upload(&p,sizeof(p),0,false,true,constants));
        const auto& previous=current.channels[request.channel].raw;
        BindingSetDesc bindings;bindings.trackLiveness=true;
        bindings.addItem(BindingSetItem::ConstantBuffer(0,constants));
        bindings.addItem(BindingSetItem::RawBuffer_SRV(0,dense));
        bindings.addItem(BindingSetItem::RawBuffer_SRV(1,previous.buffer,{previous.byteOffset,previous.byteSize}));
        bindings.addItem(BindingSetItem::StructuredBuffer_SRV(2,current.atlasMap.buffer,Format::UNKNOWN,
            {current.atlasMap.byteOffset,current.atlasMap.byteSize}));
        bindings.addItem(BindingSetItem::RawBuffer_UAV(0,selected.raw.buffer,{selected.raw.byteOffset,selected.raw.byteSize}));
        BindingSetHandle set;check(state.device->createBindingSet(bindings,state.layout,&set));
        const uint32_t groups=(selected.storage.voxelCount+255u)/256u;
        check(builder->appendDispatch(state.pipeline,set,std::min(groups,65535u),(groups+65534u)/65535u));
        check(builder->finish(output));
    });
}
} // namespace gvdb::portable

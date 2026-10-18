// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiDownsample.h"
#include <nvrhi/shader-program.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>
namespace gvdb::portable {
namespace {
using namespace nvrhi;
struct Failure { FRESULT result; };
void require(bool value,FRESULT result=FE_INVALID_ARGS) { if(!value)throw Failure{result}; }
void check(FRESULT result) {require(NVRHI_SUCCEEDED(result),result);}
template<class F> FRESULT guarded(F&& fn) noexcept {
    try{fn();return FS_OK;}catch(const Failure& e){return e.result;}
    catch(const std::bad_alloc&){return FE_OUT_OF_MEMORY;}
    catch(const std::length_error&){return FE_OUT_OF_MEMORY;}
    catch(...){return FE_GENERIC_ERROR;}
}
uint64_t product(uint64_t a,uint64_t b) { require(!a||b<=UINT64_MAX/a,FE_UNSUPPORTED);return a*b; }
uint64_t count(UInt3 r) {return product(product(r.x,r.y),r.z);}
bool finite(Float4 value) {return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z)&&std::isfinite(value.w);}
struct Identity {};
struct alignas(16) Parameters {
    slang::UInt4 sourceResolution,destinationResolution;
    Float4 destinationExtent,transformRow0,transformRow1,transformRow2,remap;
};
static_assert(sizeof(Parameters)==112);
void validate(const DownsampleRequestDesc& desc,const DownsamplePreparationBudget& budget,uint64_t& sourceCount,uint64_t& outputCount) {
    require(desc.structSize==sizeof(desc)&&!desc.reserved[0]&&!desc.reserved[1]&&!desc.reserved[2]&&
        !desc.reserved2&&!desc.reserved3&&!budget.reserved);
    require(desc.sourceResolution.x&&desc.sourceResolution.y&&desc.sourceResolution.z&&desc.sourceData);
    sourceCount=count(desc.sourceResolution);
    require(sourceCount<=UINT32_MAX/sizeof(float),FE_UNSUPPORTED);
    require(desc.sourceBytes>=sourceCount*sizeof(float));
    const auto d=desc.destinationResolution;
    require(d.x<=65535u*8u&&d.y<=65535u*8u&&d.z<=65535u*8u,FE_UNSUPPORTED);
    outputCount=count(d);
    require(outputCount<=UINT32_MAX/(sizeof(uint32_t)*2),FE_UNSUPPORTED);
    require(finite(desc.destinationExtent)&&desc.destinationExtent.x>0&&desc.destinationExtent.y>0&&desc.destinationExtent.z>0);
    require(finite(desc.remap));
    const float denominator=desc.remap.y-desc.remap.x;
    require(std::isfinite(denominator)&&denominator!=0);
    const float extents[]={desc.destinationExtent.x,desc.destinationExtent.y,desc.destinationExtent.z};
    const uint32_t dimensions[]={d.x,d.y,d.z};
    double coordinateBounds[3];
    constexpr double epsilon=std::numeric_limits<float>::epsilon();
    for(unsigned axis=0;axis<3;++axis) {
        // Bound multiply BEFORE divide, including the largest (id+1) operand.
        // Quarter-FLT_MAX leaves ample room for rounding on every backend.
        require(double(dimensions[axis]+1)*extents[axis]<=double(std::numeric_limits<float>::max())/4.0);
        coordinateBounds[axis]=std::max(1.0,double(extents[axis])*(1.0+8.0*epsilon)+1.0);
    }
    for(const auto& row:desc.transformRows) {
        require(finite(row));
        // Absolute terms bound every product and partial sum, including severe
        // cancellation. The margin exceeds the six binary32 operation errors.
        const double magnitude=std::abs(double(row.x))*coordinateBounds[0]+
            std::abs(double(row.y))*coordinateBounds[1]+std::abs(double(row.z))*coordinateBounds[2]+std::abs(double(row.w));
        require(magnitude*(1.0+16.0*epsilon)<=2147483000.0);
    }
    const uint64_t visits=product(sourceCount,outputCount);
    require(visits<=budget.maxSampleVisits,FE_OUT_OF_MEMORY);
    const uint64_t copied=outputCount?sourceCount*sizeof(float):0;
    require(copied<=budget.maxInputBytes,FE_OUT_OF_MEMORY);
    if(outputCount) {
        const uint64_t allocation=256+copied+outputCount*16;
        require(budget.maxBuffers>=4&&allocation<=budget.maxAllocatedBytes,FE_OUT_OF_MEMORY);
    }
}
void validateArtifact(IShaderProgram* program,uint32_t entry,const BindingLayoutDesc& layout) {
    ShaderEntryPointDesc desc;check(program->getEntryPointDesc(entry,desc));
    require(desc.entryParamsSize==0&&desc.threadGroupSize[0]==8&&desc.threadGroupSize[1]==8&&desc.threadGroupSize[2]==8,FE_UNSUPPORTED);
    const ResourceType types[]={ResourceType::ConstantBuffer,ResourceType::StructuredBuffer_SRV,ResourceType::StructuredBuffer_UAV};
    require(layout.bindings.size()==3,FE_UNSUPPORTED);
    for(auto type:types)require(std::count_if(layout.bindings.begin(),layout.bindings.end(),[&](const auto& item){return item.type==type&&item.slot==0;})==1,FE_UNSUPPORTED);
    uint32_t globals=0,members=0,mask=0,resourceMask=0,constant=UINT32_MAX;
    for(uint32_t i=0;i<program->getParameterCount(entry);++i) {
        ShaderParameterDesc p;check(program->getParameterDesc(entry,i,p));
        require(p.registerSpace==0&&p.count==1,FE_UNSUPPORTED);
        if(p.scope==ShaderParameterScope::Global) {
            ++globals;require(p.parentParameter==UINT32_MAX&&p.registerIndex==0,FE_UNSUPPORTED);
            const uint32_t resourceBit=p.resourceType==ResourceType::ConstantBuffer?1u:p.resourceType==ResourceType::StructuredBuffer_SRV?2u:p.resourceType==ResourceType::StructuredBuffer_UAV?4u:0u;
            require(resourceBit&&!(resourceMask&resourceBit),FE_UNSUPPORTED);resourceMask|=resourceBit;
            if(p.resourceType==ResourceType::ConstantBuffer){require(p.elementStride==sizeof(Parameters),FE_UNSUPPORTED);constant=i;}
            else if(p.resourceType==ResourceType::StructuredBuffer_SRV)require(p.elementStride==4,FE_UNSUPPORTED);
            else require(p.resourceType==ResourceType::StructuredBuffer_UAV&&p.elementStride==8,FE_UNSUPPORTED);
        } else {
            require(p.scope==ShaderParameterScope::ConstantBuffer&&p.resourceType==ResourceType::None&&p.offset<sizeof(Parameters)&&
                p.offset%16==0&&p.size==16&&p.elementStride==16,FE_UNSUPPORTED);
            const uint32_t bit=1u<<(p.offset/16);require(!(mask&bit),FE_UNSUPPORTED);mask|=bit;++members;
        }
    }
    require(globals==3&&resourceMask==7&&members==7&&mask==127&&constant!=UINT32_MAX,FE_UNSUPPORTED);
    for(uint32_t i=0;i<program->getParameterCount(entry);++i){ShaderParameterDesc p;check(program->getParameterDesc(entry,i,p));
        if(p.scope==ShaderParameterScope::ConstantBuffer)require(p.parentParameter==constant,FE_UNSUPPORTED);}
}
} // namespace
struct PreparedDownsample::Impl {
    DeviceHandle device; // Released after all GPU resources.
    std::shared_ptr<Identity> identity;
    ComputePipelineHandle pipeline;
    BindingSetHandle bindings;
    BufferHandle constants,source,statistics,staging;
    Parameters parameters{};
    UInt3 destination{};
    std::vector<float> input,result;
    uint64_t outputCount=0;
    bool recorded=false,completed=false;
};
struct NvrhiDownsample::Impl {
    DeviceHandle device;
    std::shared_ptr<Identity> identity=std::make_shared<Identity>();
    ShaderProgramHandle program;
    BindingLayoutHandle layout;
    ComputePipelineHandle pipeline;
};
PreparedDownsample::PreparedDownsample(std::unique_ptr<Impl> impl) noexcept:m_impl(std::move(impl)){}
PreparedDownsample::~PreparedDownsample()=default;
IDevice* PreparedDownsample::getDevice()const noexcept{return m_impl->device.Get();}
UInt3 PreparedDownsample::getDestinationResolution()const noexcept{return m_impl->destination;}
FRESULT PreparedDownsample::getValues(BorrowedBytes& output)const noexcept {
    if(!m_impl->completed)return FE_INVALID_ARGS;
    output={m_impl->result.empty()?nullptr:m_impl->result.data(),m_impl->result.size()*sizeof(float)};return FS_OK;
}
NvrhiDownsample::NvrhiDownsample(std::unique_ptr<Impl> impl)noexcept:m_impl(std::move(impl)){}
NvrhiDownsample::~NvrhiDownsample()=default;
IDevice* NvrhiDownsample::getDevice()const noexcept{return m_impl->device.Get();}
FRESULT NvrhiDownsample::create(const DownsampleCreateDesc& desc,std::unique_ptr<NvrhiDownsample>& output)noexcept {
    return guarded([&]{
        require(desc.structSize==sizeof(desc)&&!desc.reserved&&desc.device&&desc.artifact&&desc.artifactBytes&&desc.artifactBytes<=SIZE_MAX);
        auto impl=std::make_unique<Impl>();impl->device=desc.device;
        check(createShaderProgram(impl->device,desc.artifact,size_t(desc.artifactBytes),&impl->program));
        uint32_t entry;check(impl->program->findEntryPoint("downsampleDenseRegion",ShaderType::Compute,&entry));
        BindingLayoutDesc layout;check(impl->program->getBindingLayoutDesc(entry,0,layout));validateArtifact(impl->program,entry,layout);
        check(impl->device->createBindingLayout(layout,&impl->layout));ShaderHandle shader;check(impl->program->createShader(entry,&shader));
        check(impl->device->createComputePipeline(ComputePipelineDesc().setComputeShader(shader).addBindingLayout(impl->layout),&impl->pipeline));
        output=std::unique_ptr<NvrhiDownsample>(new NvrhiDownsample(std::move(impl)));
    });
}
FRESULT NvrhiDownsample::prepare(const DownsampleRequestDesc& desc,const DownsamplePreparationBudget& budget,
    std::unique_ptr<PreparedDownsample>& output)noexcept {
    return guarded([&]{
        uint64_t sourceCount,outputCount;validate(desc,budget,sourceCount,outputCount);
        auto ticket=std::make_unique<PreparedDownsample::Impl>();ticket->device=m_impl->device;ticket->identity=m_impl->identity;
        ticket->destination=desc.destinationResolution;ticket->outputCount=outputCount;
        if(outputCount) {
            ticket->input.resize(size_t(sourceCount));std::memcpy(ticket->input.data(),desc.sourceData,size_t(sourceCount)*sizeof(float));
            auto allocate=[&](uint64_t bytes,uint32_t stride,bool constant,bool readback,bool uav) {
                BufferDesc buffer;buffer.byteSize=bytes;buffer.structStride=stride;buffer.isConstantBuffer=constant;buffer.canHaveUAVs=uav;
                buffer.cpuAccess=readback?CpuAccessMode::Read:CpuAccessMode::None;
                buffer.initialState=constant?ResourceStates::ConstantBuffer:ResourceStates::CopyDest;buffer.keepInitialState=true;
                buffer.debugName="GVDB dense regional downsample";BufferHandle result;check(ticket->device->createBuffer(buffer,&result));require(bool(result),FE_OUT_OF_MEMORY);return result;
            };
            ticket->constants=allocate(256,0,true,false,false);ticket->source=allocate(sourceCount*4,4,false,false,false);
            ticket->statistics=allocate(outputCount*8,8,false,false,true);ticket->staging=allocate(outputCount*8,0,false,true,false);
            const auto s=desc.sourceResolution,d=desc.destinationResolution;
            ticket->parameters={{s.x,s.y,s.z,0},{d.x,d.y,d.z,0},desc.destinationExtent,
                desc.transformRows[0],desc.transformRows[1],desc.transformRows[2],desc.remap};
            BindingSetDesc bindings;bindings.trackLiveness=true;
            bindings.addItem(BindingSetItem::ConstantBuffer(0,ticket->constants));bindings.addItem(BindingSetItem::StructuredBuffer_SRV(0,ticket->source));
            bindings.addItem(BindingSetItem::StructuredBuffer_UAV(0,ticket->statistics));check(ticket->device->createBindingSet(bindings,m_impl->layout,&ticket->bindings));
            ticket->pipeline=m_impl->pipeline;
        }
        output=std::unique_ptr<PreparedDownsample>(new PreparedDownsample(std::move(ticket)));
    });
}
FRESULT NvrhiDownsample::record(RecordingContext context,PreparedDownsample& prepared)noexcept {
    return guarded([&]{
        auto& t=*prepared.m_impl;require(t.identity==m_impl->identity&&!t.recorded&&!t.completed);
        require(context.device==t.device.Get()&&context.commands&&context.commands->getDevice()==context.device);
        require(context.commands->getDesc().queueType!=CommandQueue::Copy,FE_UNSUPPORTED);
        t.recorded=true;if(!t.outputCount)return;
        std::array<uint8_t,256> constants{};std::memcpy(constants.data(),&t.parameters,sizeof(t.parameters));
        context.commands->writeBuffer(t.constants,constants.data(),constants.size());
        context.commands->writeBuffer(t.source,t.input.data(),t.input.size()*sizeof(float));
        context.commands->setComputeState(ComputeState().setPipeline(t.pipeline).addBindingSet(t.bindings));
        context.commands->dispatch((t.destination.x+7)/8,(t.destination.y+7)/8,(t.destination.z+7)/8);
        context.commands->copyBuffer(t.staging,0,t.statistics,0,t.outputCount*8);
    });
}
FRESULT NvrhiDownsample::finishAfterCallerCompletion(PreparedDownsample& prepared,BorrowedBytes& output)noexcept {
    return guarded([&]{
        auto& t=*prepared.m_impl;require(t.recorded&&!t.completed);
        std::vector<float> result(size_t(t.outputCount));
        if(t.outputCount) {
            const auto* mapped=static_cast<const uint32_t*>(t.device->mapBuffer(t.staging,CpuAccessMode::Read));require(mapped,FE_GENERIC_ERROR);
            struct Unmap { IDevice* device;IBuffer* buffer;~Unmap(){device->unmapBuffer(buffer);} } unmap{t.device,t.staging};
            for(size_t i=0;i<result.size();++i) {
                const uint32_t samples=mapped[i*2+1];require(samples&&samples<=t.input.size(),FE_GENERIC_ERROR);
                float sum;std::memcpy(&sum,mapped+i*2,sizeof(sum));result[i]=float(double(sum)/double(samples));
            }
        }
        t.result.swap(result);t.completed=true;
        output={t.result.empty()?nullptr:t.result.data(),t.result.size()*sizeof(float)};
    });
}
} // namespace gvdb::portable

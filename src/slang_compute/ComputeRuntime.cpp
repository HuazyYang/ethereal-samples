// SPDX-License-Identifier: Apache-2.0
#include "ComputeRuntime.h"
#include "../gvdb_samples/gvdb/slang/GVDBPortable.h"
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <cstring>
#include <limits>
#include <algorithm>
#include <cmath>
using namespace nvrhi;
namespace ethereal::slang {
namespace {
void check(FRESULT result, const char* operation) {
    if (NVRHI_FAILED(result)) throw std::runtime_error(std::string(operation)+" failed ("+std::to_string(result)+")");
}
BindingSetItem bind(ResourceType type, uint32_t slot, IBuffer* buffer) {
    switch(type) {
    case ResourceType::ConstantBuffer: return BindingSetItem::ConstantBuffer(slot,buffer);
    case ResourceType::StructuredBuffer_SRV: return BindingSetItem::StructuredBuffer_SRV(slot,buffer);
    case ResourceType::StructuredBuffer_UAV: return BindingSetItem::StructuredBuffer_UAV(slot,buffer);
    case ResourceType::RawBuffer_SRV: return BindingSetItem::RawBuffer_SRV(slot,buffer);
    case ResourceType::RawBuffer_UAV: return BindingSetItem::RawBuffer_UAV(slot,buffer);
    default: throw std::runtime_error("This compute runtime expects buffer bindings");
    }
}
}
ComputeRuntime::ComputeRuntime(IDevice* device,const std::filesystem::path& shaderPath)
    :m_device(device),m_shaderPath(shaderPath) {
    if(!device) throw std::invalid_argument("device");
    check(device->createCommandList(CommandListParameters(),&m_commands),"createCommandList");
}
// Leave the immediate context available to graphics/RT between submissions.
void ComputeRuntime::ensureOpen() {
    if(!m_open){m_commands->open();m_open=true;}
}
ComputeRuntime::~ComputeRuntime() {
    if(m_open)m_commands->close();
    m_device->waitForIdle();
}
ComputeRuntime::Kernel& ComputeRuntime::kernel(const char* source,const char* entry) {
    const std::string name=std::string(source)+"_"+entry;
    auto found=m_kernels.find(name);
    if(found!=m_kernels.end())return found->second;
    const auto path=m_shaderPath/(name+".bin");
    std::ifstream file(path,std::ios::binary);
    if(!file)throw std::runtime_error("Missing shader artifact: "+path.string());
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
    Kernel k;
    check(createShaderProgram(m_device,bytes.data(),bytes.size(),&k.program),"createShaderProgram");
    uint32_t index=0;
    for(;index<k.program->getEntryPointCount();++index) {
        ShaderEntryPointDesc desc;
        check(k.program->getEntryPointDesc(index,desc),"getEntryPointDesc");
        if(desc.name==entry && desc.shaderType==ShaderType::Compute){k.entryDesc=std::move(desc);break;}
    }
    if(index==k.program->getEntryPointCount())throw std::runtime_error("Compute entry missing: "+name);
    check(k.program->getBindingLayoutDesc(index,0,k.layoutDesc),"getBindingLayoutDesc");
    for(uint32_t i=0;i<k.program->getParameterCount(index);++i) {
        ShaderParameterDesc p;check(k.program->getParameterDesc(index,i,p),"getParameterDesc");
        k.parameters.push_back(std::move(p));
    }
    check(m_device->createBindingLayout(k.layoutDesc,&k.layout),"createBindingLayout");
    ShaderHandle shader;
    check(k.program->createShader(index,&shader),"createShader");
    check(m_device->createComputePipeline(ComputePipelineDesc().setComputeShader(shader).addBindingLayout(k.layout),&k.pipeline),"createComputePipeline");
    return m_kernels.emplace(name,std::move(k)).first->second;
}
void ComputeRuntime::requireRaycastKernel(const char* source,const char* entry) {
    requireVolumeKernel(source,entry,false);
}
void ComputeRuntime::requireViewKernel(const char* source,const char* entry) {
    requireVolumeKernel(source,entry,true);
}
void ComputeRuntime::requireVolumeKernel(const char* source,const char* entry,bool view) {
    auto& k=kernel(source,entry);
    bool& validated=view?k.viewLayoutValidated:k.raycastLayoutValidated;
    if(validated)return;
    const auto require=[view](bool valid){if(!valid)throw std::invalid_argument(view?"Compute kernel view ABI mismatch":"Compute kernel raycast ABI mismatch");};
    require(k.entryDesc.threadGroupSize[0]==8 && k.entryDesc.threadGroupSize[1]==8 &&
        k.entryDesc.threadGroupSize[2]==1 && k.entryDesc.entryParamsSize==0);
    const ResourceType types[]={ResourceType::ConstantBuffer,ResourceType::RawBuffer_SRV,
        ResourceType::RawBuffer_SRV,ResourceType::RawBuffer_SRV,ResourceType::StructuredBuffer_SRV,
        ResourceType::StructuredBuffer_SRV,ResourceType::StructuredBuffer_UAV,ResourceType::StructuredBuffer_UAV,
        ResourceType::ConstantBuffer};
    const uint32_t slots[]={0,0,1,2,3,4,0,1,1};
    const uint32_t strides[]={sizeof(gvdb::slang::RaycastParameters),1,1,1,sizeof(gvdb::slang::LevelInfo),16,16,4,64};
    const uint32_t count=view?9:8;
    require(k.layoutDesc.bindings.size()==count);
    for(uint32_t i=0;i<count;++i)require(std::count_if(k.layoutDesc.bindings.begin(),k.layoutDesc.bindings.end(),
        [&](const auto& binding){return binding.type==types[i]&&binding.slot==slots[i];})==1);
    uint32_t globals=0,resources=0,constantIndex[2]={UINT32_MAX,UINT32_MAX};
    for(uint32_t i=0;i<k.parameters.size();++i) {
        const auto& p=k.parameters[i];require(p.registerSpace==0&&p.count==1);
        if(p.scope!=ShaderParameterScope::Global)continue;
        ++globals;require(p.parentParameter==UINT32_MAX);
        uint32_t found=count;
        for(uint32_t j=0;j<count;++j)if(p.resourceType==types[j]&&p.registerIndex==slots[j])found=j;
        require(found<count);require(!(resources&(1u<<found))&&p.elementStride==strides[found]);resources|=1u<<found;
        if(p.resourceType==ResourceType::ConstantBuffer)constantIndex[p.registerIndex]=i;
    }
    require(globals==count&&resources==((1u<<count)-1)&&constantIndex[0]!=UINT32_MAX&&
        (!view||constantIndex[1]!=UINT32_MAX));
    uint32_t members[2]{},memberMask[2]{};
    const uint32_t bytes[2]={sizeof(gvdb::slang::RaycastParameters),64};
    for(const auto& p:k.parameters)if(p.scope!=ShaderParameterScope::Global) {
        const uint32_t parent=p.parentParameter==constantIndex[0]?0:1;
        require((!parent||view)&&p.parentParameter==constantIndex[parent]);
        require(p.scope==ShaderParameterScope::ConstantBuffer&&p.resourceType==ResourceType::None&&
            p.size==16&&p.elementStride==16&&p.offset<bytes[parent]&&p.offset%16==0);
        const uint32_t bit=1u<<(p.offset/16);require(!(memberMask[parent]&bit));memberMask[parent]|=bit;++members[parent];
    }
    for(uint32_t i=0;i<(view?2u:1u);++i)
        require(members[i]==bytes[i]/16&&memberMask[i]==((1u<<(bytes[i]/16))-1));
    validated=true;
}
BufferHandle ComputeRuntime::buffer(size_t bytes,uint32_t stride,const void* data,bool raw) {
    if(!bytes)throw std::invalid_argument("zero-sized buffer");
    BufferDesc desc;
    desc.byteSize=bytes;desc.structStride=raw?0:stride;desc.canHaveRawViews=raw;
    desc.canHaveUAVs=true;desc.initialState=ResourceStates::CopyDest;desc.keepInitialState=true;
    BufferHandle result;check(m_device->createBuffer(desc,&result),"createBuffer");
    if(data)upload(result,data,bytes);
    return result;
}
void ComputeRuntime::upload(IBuffer* target,const void* data,size_t bytes) {
    if(!target||!data||bytes>target->getDesc().byteSize)throw std::invalid_argument("upload range");
    ensureOpen();m_commands->writeBuffer(target,data,bytes);
}
void ComputeRuntime::dispatch(const char* source,const char* entry,const void* parameters,size_t parameterBytes,
    const std::vector<BindingSetItem>& bindings,uint32_t x,uint32_t y,uint32_t z) {
    if(!x||!y||!z)return;
    ensureOpen();
    auto& k=kernel(source,entry);
    BufferHandle constants;
    if(parameterBytes) {
        BufferDesc desc;desc.byteSize=(parameterBytes+255)&~size_t(255);
        desc.isConstantBuffer=true;desc.initialState=ResourceStates::ConstantBuffer;desc.keepInitialState=true;
        check(m_device->createBuffer(desc,&constants),"create constant buffer");
        // D3D11 UpdateSubresource cannot partially update a constant buffer.
        std::vector<uint8_t> padded(size_t(desc.byteSize),0);
        std::memcpy(padded.data(),parameters,parameterBytes);
        upload(constants,padded.data(),padded.size());
    }
    BindingSetDesc setDesc;
    std::vector<BufferHandle> retained;
    for(const auto& layout:k.layoutDesc.bindings) {
        auto found=std::find_if(bindings.begin(),bindings.end(),[&](const auto& b) {
            return b.type==layout.type && b.slot==layout.slot;
        });
        if(found!=bindings.end()) {setDesc.addItem(*found);continue;}
        if(layout.type==ResourceType::ConstantBuffer && layout.slot==0 && constants) {
            setDesc.addItem(BindingSetItem::ConstantBuffer(0,constants));continue;
        }
        // Slang reflects module globals even when this entry does not use them.
        // Give those globals valid zero storage; callers bind the inputs used by the selected operation.
        uint32_t stride=4;
        for(const auto& p:k.parameters)if(p.resourceType==layout.type && p.registerIndex==layout.slot)
            stride=std::max(4u,p.elementStride);
        auto& fallback=m_defaults[{layout.type,stride,layout.slot}];
        if(!fallback) {
            const bool raw=layout.type==ResourceType::RawBuffer_SRV||layout.type==ResourceType::RawBuffer_UAV;
            std::vector<uint8_t> zeros(size_t(stride)*256,0);
            fallback=buffer(zeros.size(),stride,zeros.data(),raw);
        }
        setDesc.addItem(bind(layout.type,layout.slot,fallback));
    }
    BindingSetHandle set;check(m_device->createBindingSet(setDesc,k.layout,&set),"createBindingSet");
    m_commands->setComputeState(ComputeState().setPipeline(k.pipeline).addBindingSet(set));
    m_commands->dispatch(x,y,z);
}
void ComputeRuntime::flush() {
    if(!m_open)return;
    m_commands->close();m_open=false;
    const uint64_t submission=m_device->executeCommandList(m_commands);
    // D3D11 records directly on the immediate context and has no submission ID.
    if(!submission && m_device->getGraphicsAPI()!=GraphicsAPI::D3D11)
        throw std::runtime_error("executeCommandList failed");
    if(!m_device->waitForIdle())throw std::runtime_error("GPU execution failed");
    m_device->runGarbageCollection();
}
void ComputeRuntime::read(IBuffer* source,void* data,size_t bytes) {
    if(!source||!data||bytes>source->getDesc().byteSize)throw std::invalid_argument("readback range");
    BufferHandle staging;
    check(m_device->createBuffer(BufferDesc().setByteSize(bytes).setCpuAccess(CpuAccessMode::Read)
        .setInitialState(ResourceStates::CopyDest).setKeepInitialState(true),&staging),"create readback");
    ensureOpen();m_commands->copyBuffer(staging,0,source,0,bytes);flush();
    const void* mapped=m_device->mapBuffer(staging,CpuAccessMode::Read);
    if(!mapped)throw std::runtime_error("map readback failed");
    std::memcpy(data,mapped,bytes);m_device->unmapBuffer(staging);
}
BufferHandle ComputeRuntime::exclusiveScan(IBuffer* input,uint32_t count) {
    if(!count)return {};
    const uint32_t blocks=gvdb::slang::scanBlockCount(count);
    auto output=buffer(size_t(count)*4,4),sums=buffer(size_t(blocks)*4,4);
    gvdb::slang::AlgorithmParameters params{count,0,0,0};
    dispatch("GVDBAlgorithms","scanBlocks",&params,sizeof(params),{
        BindingSetItem::StructuredBuffer_SRV(1,input),BindingSetItem::StructuredBuffer_UAV(0,output),
        BindingSetItem::StructuredBuffer_UAV(1,sums)},blocks);
    if(blocks>1) {
        auto offsets=exclusiveScan(sums,blocks);
        dispatch("GVDBAlgorithms","addScanBlockOffsets",&params,sizeof(params),{
            BindingSetItem::StructuredBuffer_SRV(2,offsets),BindingSetItem::StructuredBuffer_UAV(0,output)},blocks);
    }
    return output;
}
BufferHandle ComputeRuntime::radixSort(IBuffer* input,uint32_t count) {
    if(!count)return {};
    BufferHandle source=input;
    auto flags=buffer(size_t(count)*4,4);
    auto first=buffer(size_t(count)*sizeof(gvdb::slang::KeyValue),sizeof(gvdb::slang::KeyValue));
    auto second=buffer(first->getDesc().byteSize,sizeof(gvdb::slang::KeyValue));
    for(uint32_t bit=0;bit<64;++bit) {
        const gvdb::slang::AlgorithmParameters params{count,bit,0,0};
        dispatch("GVDBAlgorithms","radixFlags",&params,sizeof(params),{
            BindingSetItem::StructuredBuffer_SRV(0,source),BindingSetItem::StructuredBuffer_UAV(0,flags)},gvdb::slang::scanBlockCount(count));
        auto offsets=exclusiveScan(flags,count);
        BufferHandle output=bit%2==0?first:second;
        dispatch("GVDBAlgorithms","radixScatter",&params,sizeof(params),{
            BindingSetItem::StructuredBuffer_SRV(0,source),BindingSetItem::StructuredBuffer_SRV(1,offsets),
            BindingSetItem::StructuredBuffer_UAV(2,output)},gvdb::slang::scanBlockCount(count));
        source=output;
    }
    return source;
}
BufferHandle ComputeRuntime::reduce(IBuffer* input,uint32_t count) {
    if(!count) {
        gvdb::slang::Reduction identity{INFINITY,-INFINITY,0,0};
        return buffer(sizeof(identity),sizeof(identity),&identity);
    }
    bool first=true;BufferHandle source=input;
    do {
        const uint32_t blocks=gvdb::slang::scanBlockCount(count);
        const gvdb::slang::AlgorithmParameters params{count,0,0,0};
        auto output=buffer(size_t(blocks)*sizeof(gvdb::slang::Reduction),sizeof(gvdb::slang::Reduction));
        dispatch("GVDBAlgorithms",first?"reduceFloatBlocks":"reducePartialBlocks",&params,sizeof(params),{
            BindingSetItem::StructuredBuffer_SRV(first?3:4,source),BindingSetItem::StructuredBuffer_UAV(3,output)},blocks);
        source=output;count=blocks;first=false;
    }while(count>1);
    return source;
}
} // namespace ethereal::slang

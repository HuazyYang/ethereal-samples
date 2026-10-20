// Fixed resource-array reflection, subranges and retained-lifetime RT probe.
// SPDX-License-Identifier: Apache-2.0
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <nvrhi/shader-program.h>
#if ETHEREAL_TEST_CUDA
#include <nvrhi/cuda.h>
#endif
#include "../../../gvdb_samples/gvdb/slang/GVDBPortable.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
using namespace nvrhi;
namespace g=gvdb::slang;
namespace {
std::atomic<unsigned> errors{0};
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
#define OK(call) require(NVRHI_SUCCEEDED(call),#call)
struct Messages:ObjectImpl<IMessageCallback>{
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity severity,const char* text)noexcept override{
        if(severity>=MessageSeverity::Error)++errors;std::fprintf(stderr,"NVRHI: %s\n",text);
    }
};
struct Result {uint32_t words[12]{};};
static_assert(sizeof(Result)==48);
uint32_t bits(float value){uint32_t result;std::memcpy(&result,&value,4);return result;}
struct Slot {
    std::array<BufferHandle,6> buffers;
    std::array<BufferRange,6> ranges;
    Result expected;
};
Slot makeSlot(IDevice* device,ICommandList* upload,uint32_t index){
    Slot slot;
    auto create=[&](unsigned resource,const void* data,size_t bytes,uint32_t stride,size_t prefix){
        auto desc=BufferDesc().setByteSize(bytes).enableAutomaticStateTracking(ResourceStates::CopyDest);
        if(stride)desc.setStructStride(stride);else desc.setCanHaveRawViews(true);
        OK(device->createBuffer(desc,&slot.buffers[resource]));
        upload->writeBuffer(slot.buffers[resource],data,bytes);
        slot.ranges[resource]={prefix,bytes-prefix};
    };
    const uint32_t scalarCount=index+2,levelCount=index+1,transferCount=index+2;
    std::vector<float> scalars(scalarCount+64,-999);
    for(uint32_t j=0;j<scalarCount;++j)scalars[j+64]=float(index*100+j)+.25f;
    create(0,scalars.data(),scalars.size()*4,4,256);slot.expected.words[0]=bits(scalars.back());
    uint32_t totalRawBytes=0;
    for(unsigned resource: {1u,2u,4u}){
        const uint32_t count=index+resource+2;
        std::vector<uint32_t> words(count+64,0xdeadbeef);
        for(uint32_t j=0;j<count;++j)words[j+64]=resource*1000+index*100+j;
        create(resource,words.data(),words.size()*4,0,256);
        slot.expected.words[resource==4?3:resource]=words.back();totalRawBytes+=count*4;
    }
    std::vector<g::LevelInfo> levels(levelCount+4);
    for(uint32_t j=0;j<levelCount;++j){auto& level=levels[j+4];
        level.nodes.x=3000+index*100+j;level.children.y=4000+index*100+j;
        level.childExtent.z=float(5000+index*100+j);level.nodeExtent.w=int32_t(6000+index*100+j);
    }
    create(3,levels.data(),levels.size()*sizeof(g::LevelInfo),sizeof(g::LevelInfo),256);
    const auto& last=levels.back();slot.expected.words[4]=last.nodes.x+last.children.y+uint32_t(last.childExtent.z)+uint32_t(last.nodeExtent.w);
    std::vector<g::Float4> transfer(transferCount+16);
    for(uint32_t j=0;j<transferCount;++j)transfer[j+16]={float(index)+.25f,float(j)+.5f,2,3};
    create(5,transfer.data(),transfer.size()*sizeof(g::Float4),sizeof(g::Float4),256);
    const auto& t=transfer.back();slot.expected.words[5]=bits(t.x+t.y+t.z+t.w);
    slot.expected.words[7]=(4u<<24)|(64u<<12)|16u;
    slot.expected.words[8]=scalarCount;slot.expected.words[9]=levelCount;slot.expected.words[10]=transferCount;slot.expected.words[11]=totalRawBytes;
    return slot;
}
void reflection(IShaderProgram* program,uint32_t entry,bool cuda){
    const char* names[]={"scalarAtlases","nodePools","childPools","levelPools","colorAtlases","transfers"};
    const uint32_t slots[]={16,24,32,40,48,56},strides[]={4,1,1,64,1,16};
    unsigned seen=0;
    ShaderEntryPointDesc e;OK(program->getEntryPointDesc(entry,e));
    std::printf("entry globals=%u entry=%u\n",e.globalParamsSize,e.entryParamsSize);
    for(uint32_t j=0;j<program->getParameterCount(entry);++j){
        ShaderParameterDesc p;OK(program->getParameterDesc(entry,j,p));
        for(unsigned i=0;i<6;++i)if(p.name==names[i]){
            require(p.registerIndex==slots[i]&&p.registerSpace==0&&p.count==8&&p.elementStride==strides[i],"array logical register/count/element stride");
            if(cuda)require(p.offset==i*128&&p.size==128,"OptiX resource array pointer/count struct layout");
            std::printf("%s t%u count=%u offset=%u size=%u element=%u\n",p.name.c_str(),p.registerIndex,p.count,p.offset,p.size,p.elementStride);++seen;
        }
    }
    require(seen==6,"all six arrays reflected");
    if(cuda)require(e.globalParamsSize==784&&e.entryParamsSize==0,"OptiX array global parameter block size");
}
int run(IDevice* device,const char* path){
    if(!device->queryFeatureSupport(Feature::RayTracingPipeline)){std::puts("SKIP: ray tracing unavailable");return 77;}
    std::ifstream file(path,std::ios::binary);std::vector<char> bytes{std::istreambuf_iterator<char>(file),{}};
    ShaderProgramHandle program;OK(nvrhiCreateShaderProgram(device,bytes.data(),bytes.size(),&program));
    uint32_t entry=0;OK(program->findEntryPoint("arrayProbe",ShaderType::RayGeneration,&entry));
    reflection(program,entry,device->getGraphicsAPI()==GraphicsAPI::CUDA);
    BindingLayoutDesc layoutDesc;OK(program->getBindingLayoutDesc(entry,0,layoutDesc));layoutDesc.visibility=ShaderType::AllRayTracing;
    require(layoutDesc.bindings.size()==7,"fixed array layout binding count");
    BindingLayoutHandle layout;OK(device->createBindingLayout(layoutDesc,&layout));
    ShaderHandle raygen;OK(program->createShader(entry,&raygen));
    rt::PipelineHandle pipeline;OK(device->createRayTracingPipeline(rt::PipelineDesc()
        .addShader(rt::PipelineShaderDesc().setExportName("arrayProbe").setShader(raygen)).addBindingLayout(layout)
        .setMaxPayloadSize(4).setMaxAttributeSize(8).setMaxRecursionDepth(1),&pipeline));
    program=nullptr;raygen=nullptr;bytes.clear();
    for(unsigned round=0;round<2;++round){
        rt::ShaderTableHandle table;OK(pipeline->createShaderTable(
            rt::ShaderTableDesc().setMaxEntries(1).setIsCached(round!=0),&table));
        table->setRayGenerationShader("arrayProbe");
        require(table->getNumEntries()==1,"raygen-only table has one public entry and no miss shader");
        CommandListHandle upload;OK(device->createCommandList(CommandListParameters(),&upload));upload->open();
        std::array<Slot,8> slots;Slot dummy;if(round)dummy=makeSlot(device,upload,31);
        for(unsigned i=0;i<8;++i)slots[i]=round&&i>=5?dummy:makeSlot(device,upload,i);
        BufferHandle output,readback;
        OK(device->createBuffer(BufferDesc().setByteSize(8*sizeof(Result)).setStructStride(sizeof(Result)).setCanHaveUAVs(true)
            .enableAutomaticStateTracking(ResourceStates::UnorderedAccess),&output));
        OK(device->createBuffer(BufferDesc().setByteSize(8*sizeof(Result)).setCpuAccess(CpuAccessMode::Read)
            .enableAutomaticStateTracking(ResourceStates::CopyDest),&readback));
        BindingSetDesc desc;desc.addItem(BindingSetItem::StructuredBuffer_UAV(0,output));
        const uint32_t registers[]={16,24,32,40,48,56};
        for(unsigned i=0;i<8;++i)for(unsigned r=0;r<6;++r){
            auto item=(r==0||r==3||r==5)?BindingSetItem::StructuredBuffer_SRV(registers[r],slots[i].buffers[r]):
                BindingSetItem::RawBuffer_SRV(registers[r],slots[i].buffers[r]);
            item.setArrayElement(i).setRange(slots[i].ranges[r]);desc.addItem(item);
        }
        BindingSetHandle bindings;OK(device->createBindingSet(desc,layout,&bindings));
        upload->close();require(device->executeCommandList(upload)!=0,"array upload submission");
        require(device->waitForIdle(),"array upload completion");upload=nullptr;device->runGarbageCollection();
        // The binding set must now be the only owner of every input allocation.
        for(auto& slot:slots)for(auto& buffer:slot.buffers)buffer=nullptr;
        for(auto& buffer:dummy.buffers)buffer=nullptr;
        device->runGarbageCollection();
        CommandListHandle commands;OK(device->createCommandList(CommandListParameters(),&commands));commands->open();
        commands->setRayTracingState(rt::State().setShaderTable(table).addBindingSet(bindings));
        commands->dispatchRays(rt::DispatchRaysArguments().setWidth(8));
        commands->copyBuffer(readback,0,output,0,8*sizeof(Result));commands->close();
        bindings=nullptr; // Recorded commands retain the set and all array elements.
        require(device->executeCommandList(commands)!=0,"array ray-generation submission");
        require(device->waitForIdle(),"array dispatch completion");
        const auto* actual=static_cast<const Result*>(device->mapBuffer(readback,CpuAccessMode::Read));
        require(actual!=nullptr,"array readback mapping");bool matches=true;
        for(unsigned pixel=0;pixel<8;++pixel){unsigned slot=(pixel*5+3)%8;auto expected=slots[slot].expected;expected.words[6]=slot;
            for(unsigned word=0;word<12;++word)if(actual[pixel].words[word]!=expected.words[word]){
                matches=false;std::fprintf(stderr,"round%u pixel%u slot%u word%u: expected%u actual%u\n",round,pixel,slot,word,expected.words[word],actual[pixel].words[word]);
            }
        }
        device->unmapBuffer(readback);require(matches,"nonuniform fixed resource array values/dimensions");
    }
    std::puts("PASS six fixed resource arrays, nonuniform indexing, subrange counts/strides, shared dummy slots and retained lifetimes");
    return 0;
}
}
int main(int argc,char** argv){
    if(argc<3)return 1;
    donut::log::ConsoleApplicationMode();
    donut::log::SetCallback([](donut::log::Severity severity,const char* text){if(severity>=donut::log::Severity::Warning||std::strstr(text,"[Vulkan:")!=nullptr)++errors;std::fprintf(stderr,"%s\n",text);});
    try{
#if ETHEREAL_TEST_CUDA
        if(std::strcmp(argv[1],"-cuda")==0){
            auto callback=MAKE_RC_OBJ_PTR(Messages);cuda::DeviceDesc desc;desc.errorCB=callback;
            auto device=cuda::createDevice(desc);require(device!=nullptr,"CUDA device creation");
            const int result=run(device,argv[2]);return errors?1:result;
        }
#endif
        const auto api=std::strcmp(argv[1],"-vk")==0?GraphicsAPI::VULKAN:GraphicsAPI::D3D12;
        auto manager=TakeOver(donut::app::DeviceManager::Create(api));require(manager!=nullptr,"graphics backend unavailable");
        donut::app::DeviceCreationParameters params;params.enableNvrhiValidationLayer=true;params.enableRayTracingExtensions=true;
        params.enableDebugRuntime=argc>3&&std::strcmp(argv[3],"--native-validation")==0;
        require(manager->CreateHeadlessDevice(params),"graphics device creation");
        const int result=run(manager->GetDevice(),argv[2]);manager->Shutdown();return errors?1:result;
    }catch(const std::exception& e){std::fprintf(stderr,"Resource arrays: %s\n",e.what());return 1;}
}

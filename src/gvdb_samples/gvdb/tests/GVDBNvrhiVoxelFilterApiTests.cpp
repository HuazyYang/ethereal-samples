// SPDX-License-Identifier: Apache-2.0
// Public-interface fixture: never includes concrete HostVolume/NvrhiVolume headers.
#include "GVDBNvrhiVoxelFilterApi.h"
#include "GVDBNvrhiForeignVolume.h"
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <nvrhi/core/foundation.h>
#if ETHEREAL_TEST_CUDA
#include <nvrhi/cuda.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <vector>
using namespace nvrhi;
namespace g = gvdb::portable;
namespace {
std::atomic_uint problems{0};
void require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
void check(FRESULT status, const char* text) {
    if (NVRHI_FAILED(status)) throw std::runtime_error(std::string(text) + ": " + std::to_string(status));
}
struct Messages : ObjectImpl<IMessageCallback> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
        NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity severity, const char* text) noexcept override {
        if (severity >= MessageSeverity::Warning) ++problems;
        std::fprintf(stderr,"%s\n",text);
    }
};
struct Device {
    AutoPtr<donut::app::DeviceManager> manager;
    DeviceHandle device;
    AutoPtr<IMessageCallback> callback;
    Device(int argc, const char** argv) {
        bool useCuda = false, validation = false;
        for (int i = 1; i < argc; ++i) {
            useCuda |= !std::strcmp(argv[i],"-cuda"); validation |= !std::strcmp(argv[i],"--validation");
        }
        if (useCuda) {
#if ETHEREAL_TEST_CUDA
            cuda::DeviceDesc desc;
            desc.enableOptiX = false; callback = MAKE_RC_OBJ_PTR(Messages); desc.errorCB = callback;
            device = cuda::createDevice(desc);
#else
            throw std::runtime_error("CUDA backend unavailable");
#endif
        } else {
            manager = TakeOver(donut::app::DeviceManager::Create(donut::app::GetGraphicsAPIFromCommandLine(argc,argv)));
            require(bool(manager),"create graphics manager");
            donut::app::DeviceCreationParameters desc;
            desc.enableNvrhiValidationLayer = true; desc.enableDebugRuntime = validation;
            require(manager->CreateHeadlessDevice(desc),"create graphics device"); device = manager->GetDevice();
        }
        require(bool(device),"create test device");
    }
    ~Device() { if (device) device->waitForIdle(); device = nullptr; if (manager) manager->Shutdown(); }
};
template<class T> void queryIdentity(T* object) {
    const auto count = object->AddRef(); object->Release();
    AutoPtr<T> same;
    check(object->QueryInterface(uuid_of<T>(),reinterpret_cast<void**>(same.GetAddressOf())),"self QueryInterface");
    require(same.Get() == object,"self identity");
    same = nullptr;
    AutoPtr<IObject> identity, again;
    check(object->QueryInterface(uuid_of<IObject>(),reinterpret_cast<void**>(identity.GetAddressOf())),"base identity");
    check(identity->QueryInterface(uuid_of<IObject>(),reinterpret_cast<void**>(again.GetAddressOf())),"repeat base identity");
    require(identity == again,"canonical IObject identity"); identity = nullptr; again = nullptr;
    AutoPtr<IRHIObject> resource;
    check(object->QueryInterface(uuid_of<IRHIObject>(),reinterpret_cast<void**>(resource.GetAddressOf())),"RHI ancestor");
    resource = nullptr;
    void* missing = reinterpret_cast<void*>(uintptr_t(1));
    require(object->QueryInterface(uuid_of<IDevice>(),&missing) == FE_NOINTERFACE && !missing,"unsupported IID clears output");
    require(object->QueryInterface(uuid_of<T>(),nullptr) == FS_OK,"query-only IID probe");
    require(object->AddRef() == count,"queries preserve reference balance"); object->Release();
}
constexpr uint32_t channelsMask = 0x8000001fu;
constexpr g::AtlasFormat formats[] = {g::AtlasFormat::R32_FLOAT,g::AtlasFormat::RGB32_FLOAT,
    g::AtlasFormat::RGBA32_FLOAT,g::AtlasFormat::R8_UINT,g::AtlasFormat::RGBA8_UINT};
constexpr uint32_t strides[] = {4,12,16,1,4};
constexpr g::Float4 boundaries[] = {{10,0,0,0},{10,20,30,0},{10,20,30,40},{7,0,0,0},{20,40,60,99}};
struct Case {
    g::VoxelFilterRequest request;
    g::Float4 input{8,16,24,32};
    g::Float4 expected{};
    int originX = 0;
    bool noise = false;
};
std::array<uint8_t,16> encode(uint32_t channel,g::Float4 value) {
    std::array<uint8_t,16> bytes{};
    if(channel==3)bytes[0]=uint8_t(value.x);
    else if(channel==4){bytes[0]=uint8_t(value.x);bytes[1]=uint8_t(value.y);bytes[2]=uint8_t(value.z);bytes[3]=uint8_t(value.w);}
    else std::memcpy(bytes.data(),&value,strides[channel]);
    return bytes;
}
g::SnapshotHandle snapshot(g::IGVDBVolume* volume) {
    g::SnapshotHandle result;check(volume->getSnapshot(&result),"retain snapshot");return result;
}
g::ChannelSnapshotDesc channel(g::IGVDBSnapshot* state,uint32_t slot) {
    g::ChannelSnapshotDesc value;check(state->getChannel(slot,value),"inspect channel");return value;
}
void unchanged(g::IGVDBSnapshot* a,g::IGVDBSnapshot* b,uint32_t mask) {
    const auto ar=a->getDesc().revision,br=b->getDesc().revision;
    require(ar.epoch==br.epoch && ar.topology==br.topology && ar.placement==br.placement,"filter retains topology/placement");
    for(uint32_t c=0;c<32;++c)if(mask&(1u<<c)) {
        const auto ac=channel(a,c),bc=channel(b,c);
        require(ac.raw.buffer==bc.raw.buffer && ac.revision.resource==bc.revision.resource && ac.revision.content==bc.revision.content,
            "unwritten channels retain buffers and revisions");
    }
}
g::VolumeHandle volume(IDevice* device,const std::vector<char>& artifact) {
    g::VolumeCreateDesc desc;desc.device=device;desc.configuration.levelCount=1;desc.configuration.logDimension={1};
    desc.remapArtifact=artifact.data();desc.remapArtifactBytes=artifact.size();
    g::VolumeHandle result;check(g::createVolume(desc,&result),"create public volume");return result;
}
void verify(g::IGVDBReadback* readback,const Case& test,bool filtered) {
    // Fixed independent Jenkins-hash fixture values for atlas coordinates (1..2)^3,
    // in x-fast order; explicit values avoid sharing shader hashing code.
    constexpr float noise[8]={0.573661804f,0.0712890625f,0.525758266f,0.550042629f,0.525758266f,0.550042629f,0.573661804f,0.0712890625f};
    for(uint32_t c=0;c<5;++c) {
        g::BorrowedBytes bytes;check(readback->getChannelData(c,bytes),"read retained filter bytes");
        require(bytes.byteCount==128*strides[c],"logical atlas size");
        const auto* data=static_cast<const uint8_t*>(bytes.data);
        for(uint32_t z=0;z<4;++z)for(uint32_t y=0;y<4;++y)for(uint32_t x=0;x<8;++x) {
            const bool active=x<4,inside=x>=1&&x<=2&&y>=1&&y<=2&&z>=1&&z<=2;
            g::Float4 value{};
            if(active) {
                value=inside ? (c==test.request.channel ? test.input : g::Float4{4,8,12,16}) : boundaries[c];
                if(filtered && c==test.request.channel) {
                    if(inside) {
                        value=test.expected;
                        if(test.noise)value.x+=noise[(z-1)*4+(y-1)*2+x-1]*test.request.parameters.x;
                    } else if(test.request.updateApron)value=test.request.boundaryValue;
                }
            }
            const size_t offset=((z*4+y)*8+x)*strides[c];
            if(c<3) {
                for(uint32_t component=0;component<strides[c]/4;++component) {
                    float actual;std::memcpy(&actual,data+offset+4*component,4);
                    const float components[]={value.x,value.y,value.z,value.w};
                    const float expected=components[component];
                    if(!std::isfinite(actual)||std::abs(actual-expected)>2e-5f) {
                        std::fprintf(stderr,"filter op%u ch%u flags%u/%u xyz%u,%u,%u component%u actual%g expected%g\n",
                            uint32_t(test.request.operation),c,test.request.updateApron,test.request.skipOverAprons,x,y,z,component,actual,expected);
                        throw std::runtime_error("independent float filter/schedule result");
                    }
                }
            } else {
                const auto expected=encode(c,value);
                require(!std::memcmp(data+offset,expected.data(),strides[c]),"independent integer expand/alpha/apron result");
            }
        }
    }
    g::BorrowedBytes untouched;check(readback->getChannelData(31,untouched),"read slot31 sentinel");
    const auto* data=static_cast<const uint8_t*>(untouched.data);
    for(size_t i=0;i<128;++i) {
        float actual;std::memcpy(&actual,data+4*i,4);
        const uint32_t x=uint32_t(i%8),y=uint32_t(i/8%4),z=uint32_t(i/32);
        const bool inside=x>=1&&x<=2&&y>=1&&y<=2&&z>=1&&z<=2;
        require(actual==(x>=4 ? 0.f : inside ? 42.f : 13.f),"slot31 unchanged bytes");
    }
}
void oneCase(IDevice* device,const std::vector<char>& remap,const std::vector<char>& artifact,const Case& test,bool failures) {
    auto owner=volume(device,remap),foreign=volume(device,remap);
    g::VoxelFiltersCreateDesc desc;desc.volume=owner;desc.artifact=artifact.data();desc.artifactBytes=artifact.size();
    if(failures) {
        auto impostor=MAKE_RC_OBJ_PTR(gvdb_api_test::ForeignVolume);
        auto invalid=desc;invalid.volume=impostor;
        auto* out=reinterpret_cast<g::IGVDBVoxelFilters*>(uintptr_t(1));
        require(g::createVoxelFilters(invalid,&out)==FE_INVALID_ARGS && !out,"foreign public implementation rejected");
    }
    g::VoxelFiltersHandle filters;check(g::createVoxelFilters(desc,&filters),"create public filters before publication");
    require(filters->getVolume()==owner.Get(),"retained exact public owner");
    if(failures)queryIdentity(filters.Get());
    auto* target=filters->getVolume();owner=nullptr;
    if(failures) {
        auto* out=reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
        require(NVRHI_FAILED(filters->prepare(test.request,{},&out))&&!out,"prepare requires published volume");
    }
    g::NodeId leaf;check(target->activate(test.originX,0,0,leaf),"activate leaf");check(target->finishTopology(),"finish topology");
    std::array<std::vector<uint8_t>,6> bytes;
    std::array<g::ChannelUploadDesc,6> uploads;
    for(uint32_t c=0;c<6;++c) {
        const uint32_t slot=c==5 ? 31 : c,stride=c==5 ? 4 : strides[c];
        check(target->setChannel(slot,{c==5 ? g::AtlasFormat::R32_FLOAT : formats[c],c==5 ? g::Float4{13,0,0,0} : boundaries[c]}),"set raw format");
        bytes[c].assign(128*stride,233);
        const auto value=c==5 ? g::Float4{42,0,0,0} : c==test.request.channel ? test.input : g::Float4{4,8,12,16};
        const auto encoded=encode(c==5?0:c,value);
        for(uint32_t z=1;z<=2;++z)for(uint32_t y=1;y<=2;++y)for(uint32_t x=1;x<=2;++x)
            std::memcpy(bytes[c].data()+((z*4+y)*8+x)*stride,encoded.data(),stride);
        uploads[c]={slot,0,bytes[c].data(),bytes[c].size()};
    }
    g::GenerationDesc initialDesc;initialDesc.placement.minimumBrickCount={2,1,1};initialDesc.uploads=uploads.data();initialDesc.uploadCount=uploads.size();
    g::GenerationTicketHandle initialTicket;check(target->prepareGeneration(initialDesc,&initialTicket),"prepare initial raw volume");
    CommandListHandle commands;check(device->createCommandList({},&commands),"create caller list");commands->open();
    const g::RecordingContext context{device,commands};check(target->recordAndPublish(context,initialTicket),"record initial upload");
    auto initial=snapshot(target);
    if(failures) {
        auto rejected=[&](g::VoxelFilterRequest request,g::VoxelFilterPreparationBudget budget,FRESULT expected) {
            auto* out=reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
            require(filters->prepare(request,budget,&out)==expected && !out,"invalid filter clears output with classified result");
            unchanged(initial,snapshot(target),channelsMask);
        };
        auto r=test.request;r.structSize--;rejected(r,{},FE_INVALID_ARGS);
        r=test.request;r.reserved=1;rejected(r,{},FE_INVALID_ARGS);
        r=test.request;r.reserved2=1;rejected(r,{},FE_INVALID_ARGS);
        r=test.request;r.updateApron=2;rejected(r,{},FE_INVALID_ARGS);
        r=test.request;r.skipOverAprons=2;rejected(r,{},FE_INVALID_ARGS);
        r=test.request;r.operation=g::VoxelFilterOperation(999);rejected(r,{},FE_INVALID_ARGS);
        r=test.request;r.iterations=4097;rejected(r,{},FE_UNSUPPORTED);
        r=test.request;r.parameters.w=std::numeric_limits<float>::quiet_NaN();rejected(r,{},FE_INVALID_ARGS);
        r=test.request;r.boundaryValue.z=std::numeric_limits<float>::infinity();rejected(r,{},FE_INVALID_ARGS);
        r=test.request;r.channel=30;rejected(r,{},FE_NOT_FOUND);
        r=test.request;r.channel=32;rejected(r,{},FE_INVALID_ARGS);
        r=test.request;r.channel=3;rejected(r,{},FE_UNSUPPORTED);
        r=test.request;r.parameters.x=-6;rejected(r,{},FE_INVALID_ARGS);
        rejected(test.request,{UINT64_MAX,UINT32_MAX,1},FE_INVALID_ARGS);
        rejected(test.request,{UINT64_MAX,0,0},FE_OUT_OF_MEMORY);
        rejected(test.request,{UINT64_MAX,1,0},FE_OUT_OF_MEMORY);
        rejected(test.request,{0,UINT32_MAX,0},FE_OUT_OF_MEMORY);
        require(filters->prepare(test.request,{},nullptr)==FE_INVALID_ARGS,"null ticket output rejected");
    }
    g::GenerationTicketHandle prepared,stale;
    check(filters->prepare(test.request,{},&prepared),"prepare filter");
    if(failures) {
        check(filters->prepare(test.request,{},&stale),"prepare stale filter");
        require(foreign->recordAndPublish(context,prepared)==FE_INVALID_ARGS,"foreign owner rejection does not consume ticket");
        require(target->recordAndPublish({},prepared)==FE_INVALID_ARGS,"invalid context leaves ticket usable");
    }
    check(target->recordAndPublish(context,prepared),"record filter");
    auto final=snapshot(target);unchanged(initial,final,channelsMask&~(1u<<test.request.channel));
    const auto a=channel(initial,test.request.channel),b=channel(final,test.request.channel);
    require(a.raw.buffer!=b.raw.buffer && b.revision.content>a.revision.content && b.revision.resource>a.revision.resource,
        "selected channel alone receives new storage and revisions");
    require(NVRHI_FAILED(target->recordAndPublish(context,prepared)),"filter ticket consumes once");
    if(failures)require(NVRHI_FAILED(target->recordAndPublish(context,stale)),"stale filter ticket rejected");
    auto noop=test.request;noop.iterations=0;
    g::GenerationTicketHandle noWork;check(filters->prepare(noop,{0,0,0},&noWork),"zero iterations needs no allocations");
    check(target->recordAndPublish(context,noWork),"publish authenticated no-op");unchanged(final,snapshot(target),channelsMask);
    require(NVRHI_FAILED(target->recordAndPublish(context,noWork)),"no-op consumes once");
    std::array<g::ReadbackHandle,2> reads;
    check(target->prepareReadback(initial,channelsMask,&reads[0]),"prepare old generation readback");
    check(target->prepareReadback(final,channelsMask,&reads[1]),"prepare filtered readback");
    for(auto& read:reads)check(target->recordReadback(context,read),"record retained readback");
    filters=nullptr;foreign=nullptr;initial=nullptr;final=nullptr;initialTicket=nullptr;prepared=nullptr;stale=nullptr;noWork=nullptr;
    commands->close();device->executeCommandList(commands);require(device->waitForIdle(),"caller completion");
    for(uint32_t i=0;i<2;++i){check(reads[i]->finishAfterCallerCompletion(),"finish after owner/operator release");verify(reads[i],test,i!=0);}
}
void run(IDevice* device,const std::vector<char>& remap,const std::vector<char>& artifact) {
    bool first=true;
    for(uint32_t c=0;c<3;++c)for(uint32_t update=0;update<2;++update)for(uint32_t skip=0;skip<2;++skip) {
        Case test;test.request.channel=c;test.request.iterations=2;test.request.parameters={2,0,0,0};
        test.request.boundaryValue={2,4,6,8};test.request.updateApron=update;test.request.skipOverAprons=skip;
        // First pass: (2*8 + 3*8 + 3*10)/8 = 8.75. Second pass observes
        // either updated boundary2 or original10: 6.21875 versus9.21875.
        const float expected=update ? 6.21875f : 9.21875f;
        test.expected={expected,expected*2,expected*3,expected*4};
        oneCase(device,remap,artifact,test,first);first=false;
    }
    { Case test;test.request.channel=2;test.request.parameters={1,.25f,0,0};
      test.expected={62.f/7+.25f,124.f/7+.25f,186.f/7+.25f,248.f/7+.25f};
      oneCase(device,remap,artifact,test,false); }
    for(float initial:{8.f,0.f}) {
        Case test;test.request.operation=g::VoxelFilterOperation::Grow;test.request.parameters={-.2f,0,0,0};
        test.input={initial,0,0,0};test.expected={initial==0?0.f:6.f,0,0,0};oneCase(device,remap,artifact,test,false);
    }
    { Case test;test.request.operation=g::VoxelFilterOperation::Noise;test.request.parameters={.5f,0,0,0};
      test.expected={8,0,0,0};test.noise=true;oneCase(device,remap,artifact,test,false); }
    for(int origin:{0,52}) {
        Case test;test.request.operation=g::VoxelFilterOperation::Cut;test.input={1,0,0,0};
        test.originX=origin;test.expected={origin==0?.02f:1.f,0,0,0};oneCase(device,remap,artifact,test,false);
    }
    for(uint32_t skip=0;skip<2;++skip) {
        Case test;test.request.channel=3;test.request.operation=g::VoxelFilterOperation::ExpandByte;
        test.request.parameters={7,9,0,0};test.request.skipOverAprons=skip;test.input={};test.expected={9,0,0,0};
        oneCase(device,remap,artifact,test,false);
    }
    for(bool normalize:{false,true}) {
        Case test;test.request.channel=4;test.request.operation=g::VoxelFilterOperation::ExpandColor;
        test.request.parameters=normalize?g::Float4{10,5,0,0}:g::Float4{1,.5f,0,0};
        test.request.boundaryValue={17,18,19,20};test.input={10,20,30,99};
        test.expected=normalize?g::Float4{85,170,255,1}:g::Float4{55,110,165,1};oneCase(device,remap,artifact,test,false);
    }
    std::puts("Public filters: six operations, five formats, flags/iteration schedule, no-ops, failures, revisions and retained lifetimes PASS");
}
std::vector<char> readFile(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);require(bool(file),"open offline filter artifact");
    return {(std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>()};
}
}
int main(int argc,const char** argv) {
    donut::log::ConsoleApplicationMode();
    donut::log::SetCallback([](donut::log::Severity severity,const char* text){if(severity>=donut::log::Severity::Warning)++problems;std::fprintf(stderr,"%s\n",text);});
    try {
        {
            Device device(argc,argv);const auto root=donut::app::GetDirectoryWithExecutable()/"shaders";
            const auto type=donut::app::GetShaderTypeName(device.device->getGraphicsAPI());
            auto remap=root/"gvdb_owner"/type/"GVDBAtlasRepack_relocateAtlas.bin";
            auto filters=root/"gvdb_filters"/type/"GVDBAtlasFilters_filterAtlas.bin";
            for(int i=1;i+1<argc;++i){if(!std::strcmp(argv[i],"--artifact"))remap=argv[i+1];if(!std::strcmp(argv[i],"--filters"))filters=argv[i+1];}
            run(device.device,readFile(remap),readFile(filters));
        }
        require(problems==0,"native/NVRHI warnings or errors");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}

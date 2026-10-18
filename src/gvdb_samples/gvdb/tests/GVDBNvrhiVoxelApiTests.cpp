// SPDX-License-Identifier: Apache-2.0
// Public-interface fixture: never includes concrete HostVolume/NvrhiVolume headers.
#include "GVDBNvrhiVoxelApi.h"
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
constexpr uint32_t channelMask = 0x1f1fu;
constexpr g::AtlasFormat formats[] = {g::AtlasFormat::R8_UINT,g::AtlasFormat::RGBA8_UINT,
    g::AtlasFormat::R32_FLOAT,g::AtlasFormat::RGB32_FLOAT,g::AtlasFormat::RGBA32_FLOAT};
constexpr uint32_t strides[] = {1,4,4,12,16};
constexpr g::Float4 fills[] = {{17.75f,0,0,0},{.25f,.5f,.75f,.125f},{3.5f,0,0,0},
    {1.25f,-2.5f,7.75f,1},{2.25f,-4.5f,9.5f,-100}};
constexpr g::Float4 storedFills[] = {{17,0,0,0},{63,127,191,255},{3.5f,0,0,0},
    {1.25f,-2.5f,7.75f,0},{2.25f,-4.5f,9.5f,1}};
g::Float4 voidValue(bool destination) { return destination ? g::Float4{23,24,25,26} : g::Float4{7,8,9,10}; }
std::array<uint8_t,16> encode(uint32_t format, g::Float4 value) {
    std::array<uint8_t,16> bytes{};
    if (format == 0) bytes[0] = uint8_t(value.x);
    else if (format == 1) { bytes[0]=uint8_t(value.x); bytes[1]=uint8_t(value.y); bytes[2]=uint8_t(value.z); bytes[3]=uint8_t(value.w); }
    else std::memcpy(bytes.data(),&value,strides[format]);
    return bytes;
}
g::VolumeHandle volume(IDevice* device, const std::vector<char>& artifact) {
    g::VolumeCreateDesc desc;
    desc.device=device; desc.configuration.levelCount=2; desc.configuration.logDimension={1,1};
    desc.remapArtifact=artifact.data(); desc.remapArtifactBytes=artifact.size();
    g::VolumeHandle result; check(g::createVolume(desc,&result),"create public volume"); return result;
}
g::SnapshotHandle snapshot(g::IGVDBVolume* volume) {
    g::SnapshotHandle result; check(volume->getSnapshot(&result),"retain public snapshot"); return result;
}
g::ChannelSnapshotDesc channel(g::IGVDBSnapshot* snapshot, uint32_t index) {
    g::ChannelSnapshotDesc result; check(snapshot->getChannel(index,result),"inspect public channel"); return result;
}
void equalBindings(g::IGVDBSnapshot* a,g::IGVDBSnapshot* b,uint32_t mask) {
    const auto ar=a->getDesc().revision, br=b->getDesc().revision;
    require(ar.epoch==br.epoch && ar.topology==br.topology && ar.placement==br.placement,"operator keeps topology/placement revisions");
    for (uint32_t i=0;i<32;++i) if (mask&(1u<<i)) {
        const auto ac=channel(a,i),bc=channel(b,i);
        require(ac.raw.buffer==bc.raw.buffer && ac.raw.byteSize==bc.raw.byteSize &&
            ac.revision.resource==bc.revision.resource && ac.revision.content==bc.revision.content,"unchanged channel retains bindings/revisions");
    }
}
void verify(g::IGVDBReadback* readback,uint32_t stage) {
    require(readback->getChannelMask()==channelMask,"readback selected all ten channels");
    for (uint32_t destination=0;destination<2;++destination) for (uint32_t format=0;format<5;++format) {
        g::BorrowedBytes bytes; check(readback->getChannelData(destination*8+format,bytes),"read operator bytes");
        require(bytes.data && bytes.byteCount==192*strides[format],"operator logical byte count");
        const auto* actual=static_cast<const uint8_t*>(bytes.data);
        for (uint32_t z=0;z<4;++z) for (uint32_t y=0;y<4;++y) for (uint32_t x=0;x<12;++x) {
            std::array<uint8_t,16> expected{};
            if (x<8) {
                g::Float4 value = destination ? (stage==0 ? voidValue(true) : storedFills[format]) :
                    (stage<2 ? storedFills[format] : g::Float4{});
                if (stage==3) {
                    const int wx=int((x/4)*2+x%4)-1,wy=int(y)-1,wz=int(z)-1;
                    if (wx<0 || wx>=4 || wy<0 || wy>=2 || wz<0 || wz>=2) value=voidValue(destination!=0);
                }
                expected=encode(format,value);
            }
            const size_t offset=((size_t(z)*4+y)*12+x)*strides[format];
            if (std::memcmp(actual+offset,expected.data(),strides[format])) {
                std::fprintf(stderr,"byte mismatch stage=%u channel=%u voxel=(%u,%u,%u)\n",stage,destination*8+format,x,y,z);
                throw std::runtime_error("independent fill/copy/clear/apron or unused-slot bytes");
            }
        }
    }
}
void factoryFailures() {
    g::VoxelOpsCreateDesc desc;
    auto* sentinel=reinterpret_cast<g::IGVDBVoxelOps*>(uintptr_t(1)); auto* output=sentinel;
    require(g::gvdbCreateNvrhiVoxelOps(g::c_GVDBNvrhiApiVersion+1,&desc,&output)==FE_UNSUPPORTED && !output,"voxel factory version");
    desc.structSize--; output=sentinel;
    require(g::createVoxelOps(desc,&output)==FE_INVALID_ARGS && !output,"voxel factory descriptor size");
    desc={};desc.reserved=1;output=sentinel;
    require(g::createVoxelOps(desc,&output)==FE_INVALID_ARGS && !output,"voxel factory reserved field");
    desc={};output=sentinel;
    require(g::createVoxelOps(desc,&output)==FE_INVALID_ARGS && !output,"voxel factory missing owner/artifact");
    require(g::createVoxelOps(desc,nullptr)==FE_INVALID_ARGS,"voxel factory null output");
}
void run(IDevice* device,const std::vector<char>& remap,const std::vector<char>& artifact) {
    auto owner=volume(device,remap),foreign=volume(device,remap);
    g::VoxelOpsCreateDesc desc;
    desc.volume=owner;desc.artifact=artifact.data();desc.artifactBytes=artifact.size();
    auto impostor=MAKE_RC_OBJ_PTR(gvdb_api_test::ForeignVolume);
    auto foreignDesc=desc;foreignDesc.volume=impostor;
    auto* foreignOutput=reinterpret_cast<g::IGVDBVoxelOps*>(uintptr_t(1));
    require(g::createVoxelOps(foreignDesc,&foreignOutput)==FE_INVALID_ARGS && !foreignOutput,
            "unrelated public volume implementation rejected before borrowing owner");
    queryIdentity(static_cast<g::IGVDBVolume*>(impostor.Get()));impostor=nullptr;
    g::VoxelOpsHandle ops;check(g::createVoxelOps(desc,&ops),"create retained voxel operators before publication");
    queryIdentity(ops.Get()); require(ops->getVolume()==owner.Get(),"operator retains exact public volume identity");
    auto* target=ops->getVolume(); owner=nullptr; // Only the operator now owns this volume.
    g::IGVDBGenerationTicket* rejected=reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
    require(NVRHI_FAILED(ops->prepareClear(0,{},&rejected)) && !rejected,"preparation requires publication and clears output");
    g::NodeId leaf;
    check(target->activate(0,0,0,leaf),"activate first leaf"); check(target->activate(2,0,0,leaf),"activate adjacent leaf");
    check(target->finishTopology(),"finish public CPU topology");
    for (uint32_t destination=0;destination<2;++destination) for (uint32_t i=0;i<5;++i)
        check(target->setChannel(destination*8+i,{formats[i],voidValue(destination!=0)}),"configure ten raw channels");
    g::GenerationDesc request;request.placement.minimumBrickCount={3,1,1};
    g::GenerationTicketHandle first;
    check(target->prepareGeneration(request,&first),"prepare initial volume");
    CommandListHandle commands;check(device->createCommandList({},&commands),"create caller list");commands->open();
    const g::RecordingContext context{device,commands};check(target->recordAndPublish(context,first),"record initial volume");
    auto initial=snapshot(target);
    auto fails=[&](auto&& operation,const char* text) {
        auto* output=reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
        require(NVRHI_FAILED(operation(&output)) && !output,text);
        equalBindings(initial,snapshot(target),channelMask);
    };
    fails([&](auto** output) { return ops->prepareFill(31,{1,0,0,0},{},output); },"missing fill channel rejected");
    fails([&](auto** output) { return ops->prepareFill(0,{std::numeric_limits<float>::quiet_NaN(),0,0,0},{},output); },"nonfinite fill rejected");
    fails([&](auto** output) { return ops->prepareFill(0,{256,0,0,0},{},output); },"out-of-range R8 fill rejected");
    fails([&](auto** output) { return ops->prepareCopy(0,9,{},output); },"cross-format copy rejected");
    fails([&](auto** output) { return ops->prepareClear(1u<<31,{},output); },"missing clear mask rejected");
    fails([&](auto** output) { return ops->prepareFill(0,{1,0,0,0},{UINT64_MAX,0,0},output); },"preparation allocation budget rejected atomically");
    fails([&](auto** output) { return ops->prepareClear(0,{UINT64_MAX,UINT32_MAX,1},output); },"reserved public budget rejected");
    g::GenerationTicketHandle stale;
    check(ops->prepareFill(4,fills[4],{},&stale),"prepare deliberately stale operator ticket");
    std::array<g::SnapshotHandle,4> snapshots;
    for (uint32_t i=0;i<5;++i) {
        g::GenerationTicketHandle ticket;check(ops->prepareFill(i,fills[i],{},&ticket),"prepare format fill");
        if (i==0) {
            require(foreign->recordAndPublish(context,ticket)==FE_INVALID_ARGS,"other owner rejects operator ticket");
            require(target->recordAndPublish({},ticket)==FE_INVALID_ARGS,"missing context leaves ticket usable");
        }
        check(target->recordAndPublish(context,ticket),"record format fill");
        if (i==0) {
            equalBindings(initial,snapshot(target),channelMask&~1u);
            require(channel(initial,0).raw.buffer!=channel(snapshot(target),0).raw.buffer,"written channel receives fresh allocation");
            require(NVRHI_FAILED(target->recordAndPublish(context,stale)),"stale operator ticket rejected");
        }
    }
    snapshots[0]=snapshot(target);
    for (uint32_t i=0;i<5;++i) {
        g::GenerationTicketHandle ticket;check(ops->prepareCopy(i,i+8,{},&ticket),"prepare same-format channel copy");
        check(target->recordAndPublish(context,ticket),"record channel copy");
    }
    snapshots[1]=snapshot(target);equalBindings(snapshots[0],snapshots[1],0x1fu);
    g::GenerationTicketHandle clear;check(ops->prepareClear(0x1f,{},&clear),"prepare clear all source formats");
    check(target->recordAndPublish(context,clear),"record source clear");
    snapshots[2]=snapshot(target);equalBindings(snapshots[1],snapshots[2],0x1f00u);
    g::GenerationTicketHandle apron;check(ops->prepareApron(channelMask,{},&apron),"prepare all-format apron");
    check(target->recordAndPublish(context,apron),"record all-format apron");snapshots[3]=snapshot(target);
    for (uint32_t noop=0;noop<3;++noop) {
        g::GenerationTicketHandle ticket;
        if (noop==0) check(ops->prepareClear(0,{0,0,0},&ticket),"prepare zero-mask clear");
        else if (noop==1) check(ops->prepareCopy(0,0,{0,0,0},&ticket),"prepare same-channel copy");
        else check(ops->prepareApron(0,{0,0,0},&ticket),"prepare zero-mask apron");
        check(target->recordAndPublish(context,ticket),"record no-op ticket");
        equalBindings(snapshots[3],snapshot(target),channelMask);
        require(NVRHI_FAILED(target->recordAndPublish(context,ticket)),"no-op consumes ticket once");
    }
    std::array<g::ReadbackHandle,4> readbacks;
    for (uint32_t i=0;i<4;++i) {
        check(target->prepareReadback(snapshots[i],channelMask,&readbacks[i]),"prepare retained operation readback");
        check(target->recordReadback(context,readbacks[i]),"record retained operation readback");
    }
    ops=nullptr;foreign=nullptr;initial=nullptr;first=nullptr;stale=nullptr;clear=nullptr;apron=nullptr;
    for(auto& held:snapshots)held=nullptr;
    commands->close();device->executeCommandList(commands);require(device->waitForIdle(),"caller completion");
    for(uint32_t i=0;i<4;++i){check(readbacks[i]->finishAfterCallerCompletion(),"finish after operator/owner destruction");verify(readbacks[i],i);}
    std::puts("GVDB public voxel API: five-format fill/copy/clear/apron, snapshots, no-ops, failures and retained owner lifetime PASS");
}
std::vector<char> readFile(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);require(bool(file),"open offline voxel artifact");
    return {(std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>()};
}
}
int main(int argc,const char** argv) {
    donut::log::ConsoleApplicationMode();
    donut::log::SetCallback([](donut::log::Severity severity,const char* text){if(severity>=donut::log::Severity::Warning)++problems;std::fprintf(stderr,"%s\n",text);});
    try {
        factoryFailures();
        {
            Device device(argc,argv);const auto root=donut::app::GetDirectoryWithExecutable()/"shaders";
            const auto type=donut::app::GetShaderTypeName(device.device->getGraphicsAPI());
            auto remap=root/"gvdb_owner"/type/"GVDBAtlasRepack_relocateAtlas.bin";
            auto operators=root/"gvdb_voxel"/type/"GVDBAtlasMutation_mutateAtlas.bin";
            for(int i=1;i+1<argc;++i){if(!std::strcmp(argv[i],"--artifact"))remap=argv[i+1];if(!std::strcmp(argv[i],"--operators"))operators=argv[i+1];}
            run(device.device,readFile(remap),readFile(operators));
        }
        require(problems==0,"native/NVRHI warnings or errors");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}

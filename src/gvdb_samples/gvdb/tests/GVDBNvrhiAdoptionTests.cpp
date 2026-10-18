// SPDX-License-Identifier: Apache-2.0
// Test-only device harness. The public owner has no Donut or native SDK includes.
#include "GVDBNvrhiVoxelOps.h"
#include "GVDBNvrhiAdoptionInternal.h"
#include <limits>
#include <cmath>
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <nvrhi/core/foundation.h>
#if ETHEREAL_TEST_CUDA
#include <nvrhi/cuda.h>
#endif
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <tuple>
using namespace nvrhi;
namespace g = gvdb::portable;
namespace {
std::atomic_uint problems{0};
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void check(FRESULT result, const char* message) {
    if (NVRHI_FAILED(result)) throw std::runtime_error(std::string(message) + ": " + std::to_string(result));
}
#if ETHEREAL_TEST_CUDA
struct Messages : ObjectImpl<IMessageCallback> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity severity, const char* text) noexcept override {
        if (severity >= MessageSeverity::Warning) ++problems;
        std::fprintf(stderr, "%s\n", text);
    }
};
#endif
struct Device {
    AutoPtr<donut::app::DeviceManager> manager;
    DeviceHandle device;
    AutoPtr<IMessageCallback> callback;
    Device(int argc, const char** argv) {
        bool cuda = false, validation = false;
        for (int i = 1; i < argc; ++i) {
            cuda |= std::strcmp(argv[i], "-cuda") == 0;
            validation |= std::strcmp(argv[i], "--validation") == 0;
        }
        if (cuda) {
#if ETHEREAL_TEST_CUDA
            cuda::DeviceDesc desc;
            desc.enableOptiX = false;
            callback = MAKE_RC_OBJ_PTR(Messages);
            desc.errorCB = callback;
            device = cuda::createDevice(desc);
#else
            throw std::runtime_error("CUDA backend unavailable in this build");
#endif
        } else {
            manager = TakeOver(donut::app::DeviceManager::Create(donut::app::GetGraphicsAPIFromCommandLine(argc, argv)));
            require(bool(manager), "requested graphics backend unavailable");
            donut::app::DeviceCreationParameters parameters;
            parameters.enableNvrhiValidationLayer = true;
            parameters.enableCopyQueue = true;
            parameters.enableDebugRuntime = validation;
            require(manager->CreateHeadlessDevice(parameters), "create graphics device");
            device = manager->GetDevice();
        }
        require(bool(device), "create test device");
    }
    ~Device() { if (device) device->waitForIdle(); device = nullptr; if (manager) manager->Shutdown(); }
};
struct Harness {
    IDevice* device;
    CommandListHandle commands;
    explicit Harness(IDevice* value) : device(value) { check(device->createCommandList({}, &commands), "create command list"); }
    g::RecordingContext context() const { return {device, commands}; }
    void begin() { commands->open(); }
    void submit() {
        commands->close();
        const auto id = device->executeCommandList(commands);
        require(id || device->getGraphicsAPI() == GraphicsAPI::D3D11, "submit");
        require(device->waitForIdle(), "GPU completion");
        device->runGarbageCollection();
    }
    std::vector<uint8_t> read(const g::BufferView& view) {
        if (!view.byteSize) return {};
        BufferHandle staging;
        check(device->createBuffer(BufferDesc().setByteSize(view.byteSize).setCpuAccess(CpuAccessMode::Read)
            .setInitialState(ResourceStates::CopyDest).setKeepInitialState(true), &staging), "staging");
        begin(); commands->copyBuffer(staging, 0, view.buffer, view.byteOffset, view.byteSize); submit();
        const void* data = device->mapBuffer(staging, CpuAccessMode::Read);
        require(data != nullptr, "map staging");
        std::vector<uint8_t> result(size_t(view.byteSize));
        std::memcpy(result.data(), data, result.size()); device->unmapBuffer(staging);
        return result;
    }
};


namespace detail = g::detail;
using Channels = std::array<std::vector<uint8_t>,5>;
const g::ChannelDesc descriptors[] = {
    {g::AtlasFormat::R8_UINT,{23,0,0,0}},{g::AtlasFormat::RGBA8_UINT,{153,51,204,117}},
    {g::AtlasFormat::R32_FLOAT,{-3.5f,0,0,0}},{g::AtlasFormat::RGB32_FLOAT,{-1.25f,2.5f,7.75f,0}},
    {g::AtlasFormat::RGBA32_FLOAT,{3.25f,-4.5f,1.75f,9.5f}}
};
struct Tag final : ObjectImpl<IRHIObject> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Tag)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    uint32_t value;
    explicit Tag(uint32_t v):value(v){}
};
std::unique_ptr<g::HostVolume> candidate() {
    auto result=std::make_unique<g::HostVolume>();g::LevelConfig config;config.levelCount=3;
    config.logDimension[0]=config.logDimension[1]=config.logDimension[2]=1;
    check(result->configure(config),"configure candidate");g::NodeId node;
    check(result->activate(-2,0,0,node),"candidate negative leaf");
    check(result->activate(0,0,0,node),"candidate adjacent leaf");
    check(result->activate(2,0,0,node),"candidate inactive leaf");check(result->setActive(node,false),"candidate inactive flag");
    result->editNode(result->findLeaf({-2,0,0}))->atlasInterior={77,88,99};
    check(result->finishTopology(),"candidate finish");return result;
}
Channels poison(uint32_t seed) {
    Channels result;
    for(uint32_t c=0;c<5;++c) {
        g::ChannelStorage storage;check(g::describeChannel(descriptors[c].format,{18,12,6},storage).result,"channel extent");
        auto& bytes=result[c];bytes.resize(size_t(storage.logicalBytes));
        for(size_t i=0;i<bytes.size();++i)bytes[i]=uint8_t(i*29+(i>>3)*11+c*37+seed);
        if(c>=2)for(size_t i=0;i+4<=bytes.size();i+=4){
            uint32_t bits=(i/4)%3==0?0x7fc12345u:((i/4)%3==1?0x80000000u:uint32_t(i*17+seed));
            std::memcpy(bytes.data()+i,&bits,4);
        }
    }
    return result;
}
void equalTopology(const gvdb::slang::PackedVolume& a,const gvdb::slang::PackedVolume& b) {
    require(std::memcmp(&a.volume,&b.volume,sizeof(a.volume))==0 && a.nodes==b.nodes && a.children==b.children &&
        a.levels.size()==b.levels.size() && a.atlasMap.size()==b.atlasMap.size(),"frozen topology sizes/bytes changed");
    require(std::memcmp(a.levels.data(),b.levels.data(),a.levels.size()*sizeof(g::LevelInfo))==0 &&
        std::memcmp(a.atlasMap.data(),b.atlasMap.data(),a.atlasMap.size()*sizeof(gvdb::slang::AtlasNode))==0,
        "frozen placement or levels changed");
}
std::unique_ptr<g::PreparedReadback> readLater(Harness& h,g::NvrhiVolume& owner,std::shared_ptr<const g::VolumeGeneration> source) {
    std::unique_ptr<g::PreparedReadback> ticket;check(owner.prepareReadback(source,source->bindings().channelMask,ticket),"prepare imported readback");
    check(owner.recordReadback(h.context(),*ticket),"record imported readback");return ticket;
}
void verify(g::PreparedReadback& ticket,const Channels& expected) {
    g::ChannelReadback bytes;check(g::NvrhiVolume::finishReadbackAfterCompletion(ticket,bytes),"read imported bytes");
    require(bytes.channelMask==31,"import channel order/mask");
    for(uint32_t c=0;c<5;++c)require(bytes.channels[c]==expected[c],"import changed logical apron/unused/raw bytes");
}
void run(IDevice* device,const std::vector<char>& artifact,const std::vector<char>& mutation) {
    Harness h(device);g::VolumeOwnerDesc desc;desc.device=device;desc.configuration.levelCount=2;
    desc.configuration.logDimension[0]=2;desc.configuration.logDimension[1]=1;
    desc.remapArtifact=artifact.data();desc.remapArtifactBytes=artifact.size();
    std::unique_ptr<g::NvrhiVolume> owner;check(g::NvrhiVolume::create(desc,owner),"owner");
    auto* stableHost=&owner->host();check(owner->setChannel(31,descriptors[1]),"old channel");
    g::NodeId oldNode;check(owner->host().activate(0,0,0,oldNode),"old node");check(owner->host().finishTopology(),"old finish");
    g::GenerationRequest dense;dense.placement.minimumBrickCount={1,1,1};dense.placement.apron=1;
    std::unique_ptr<g::PreparedGeneration> ticket;check(owner->prepareGeneration(dense,ticket),"old generation");
    h.begin();check(owner->recordAndPublish(h.context(),*ticket),"old publish");h.submit();auto old=owner->snapshot();
    auto source=candidate();const auto sourceRevision=source->revision();
    const auto first=source->findLeaf({-2,0,0}),second=source->findLeaf({0,0,0});
    detail::ImportedLeafSlot slots[]={{uint32_t(first.index()),5},{uint32_t(second.index()),2}};
    Channels bytes=poison(19);const Channels original=bytes;
    std::array<detail::ImportedChannel,5> channels;
    for(uint32_t c=0;c<5;++c)channels[c]={c,descriptors[c],bytes[c].data(),bytes[c].size()};
    detail::VolumeImportRequest request;request.candidate=source.get();request.placement={{3,2,1},2,slots,2};
    request.channels=channels.data();request.channelCount=5;request.attachment=MAKE_RC_OBJ_PTR(Tag,101u);
    auto* metadata=request.attachment.Get();
    check(owner->prepareGeneration(dense,ticket),"preserved output ticket");auto* preserved=ticket.get();
    const auto oldHostRevision=owner->host().revision();
    const auto reject=[&](const detail::VolumeImportRequest& bad){
        require(NVRHI_FAILED(detail::VolumeAdoptionBuilder::prepareImport(*owner,bad,ticket)) && ticket.get()==preserved &&
            owner->snapshot()==old && &owner->host()==stableHost && owner->host().revision().epoch==oldHostRevision.epoch &&
            owner->host().revision().topology==oldHostRevision.topology && owner->host().configuration().logDimension[0]==2,
            "failed import changed output/host/current");
    };
    auto bad=request;bad.budget.maxBuffers=6;reject(bad); // topology4 plus two channels, then failure
    auto badSlots=std::array<detail::ImportedLeafSlot,2>{slots[0],slots[0]};bad=request;bad.placement.leaves=badSlots.data();reject(bad);
    badSlots={slots[0],slots[1]};badSlots[1].slot=slots[0].slot;reject(bad);
    badSlots[1]=slots[1];badSlots[1].slot=6;reject(bad);
    bad=request;bad.placement.leafCount=1;reject(bad);
    bad=request;bad.placement.brickCount.x=0;reject(bad);
    auto badChannels=channels;badChannels[2].byteCount--;bad=request;bad.channels=badChannels.data();reject(bad);
    badChannels=channels;badChannels[2].channel=0;reject(bad);
    badChannels=channels;badChannels[2].desc.voidValue.x=std::numeric_limits<float>::quiet_NaN();reject(bad);
    check(detail::VolumeAdoptionBuilder::prepareImport(*owner,request,ticket),"prepare exact import");
    require(source->revision().epoch==sourceRevision.epoch && source->revision().topology==sourceRevision.topology &&
        source->node(first)->atlasInterior.x==77,"preparation mutated caller candidate");
    // Mutating every caller input after preparation must not alter the private staged state.
    for(auto& channel:bytes)std::fill(channel.begin(),channel.end(),0);check(source->clear(),"clear caller candidate after preparation");
    h.begin();check(owner->recordAndPublish(h.context(),*ticket),"record exact import");auto imported=owner->snapshot();
    auto importedRead=readLater(h,*owner,imported);h.submit();verify(*importedRead,original);
    require(&owner->host()==stableHost && owner->host().configuration().logDimension[0]==1 &&
        owner->host().activeLeafCount()==2 && owner->host().revision().epoch>oldHostRevision.epoch &&
        owner->host().revision().topology>oldHostRevision.topology && imported->bindings().channelMask==31,
        "adoption host identity/config/revisions");
    require(owner->host().node(first)->atlasInterior.x==-1 && detail::VolumeAdoptionBuilder::attachment(*imported)==metadata,
        "CPU placement or attachment policy");
    require(detail::VolumeAdoptionBuilder::validateGeneration(*owner,*imported)==FS_OK,"owned generation rejected");
    std::unique_ptr<g::NvrhiVolume> foreign;check(g::NvrhiVolume::create(desc,foreign),"foreign owner");
    require(detail::VolumeAdoptionBuilder::validateGeneration(*foreign,*imported)==FE_INVALID_ARGS,"foreign generation accepted");
    gvdb::slang::PackedVolume frozen;check(detail::VolumeAdoptionBuilder::exportTopology(*imported,frozen),"export imported topology");
    require(frozen.atlasMap.size()==6 && frozen.atlasMap[5].leaf==int(first.index()) && frozen.atlasMap[2].leaf==int(second.index()) &&
        frozen.atlasMap[0].leaf<0 && frozen.atlasMap[1].leaf<0 && frozen.atlasMap[3].leaf<0 && frozen.atlasMap[4].leaf<0,
        "original non-dense slots not preserved");
    g::Node placedFirst;std::memcpy(&placedFirst,reinterpret_cast<const uint8_t*>(frozen.nodes.data())+
        frozen.levels[0].nodes.x+first.index()*sizeof(g::Node),sizeof(placedFirst));
    require(placedFirst.atlasInterior.x==14 && placedFirst.atlasInterior.y==8 && placedFirst.atlasInterior.z==2,
        "original leaf interior not preserved");
    // A later target host commit rejects the staged transaction even with unchanged content.
    source=candidate();bytes=poison(73);for(uint32_t c=0;c<5;++c)channels[c]={c,descriptors[c],bytes[c].data(),bytes[c].size()};
    request.candidate=source.get();request.attachment=MAKE_RC_OBJ_PTR(Tag,202u);auto* nextMetadata=request.attachment.Get();
    std::unique_ptr<g::PreparedGeneration> stale;check(detail::VolumeAdoptionBuilder::prepareImport(*owner,request,stale),"stale import prep");
    check(owner->host().finishTopology(),"advance host revision");h.begin();
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(),*stale)) && owner->snapshot()==imported,"stale host import overwrote state");
    check(owner->prepareGeneration(dense,ticket),"same epoch repack");check(owner->recordAndPublish(h.context(),*ticket),"same epoch repack record");
    require(detail::VolumeAdoptionBuilder::attachment(*owner->snapshot())==metadata,"same epoch repack lost metadata");h.submit();
    gvdb::slang::PackedVolume after;check(detail::VolumeAdoptionBuilder::exportTopology(*imported,after),"export old after repack");equalTopology(frozen,after);
    std::unique_ptr<g::NvrhiVoxelOps> ops;check(g::NvrhiVoxelOps::create({owner.get(),mutation.data(),mutation.size()},ops),"mutation factory");
    h.begin();check(ops->prepareFill(0,{42,0,0,0},{},ticket),"metadata mutation");check(owner->recordAndPublish(h.context(),*ticket),"metadata mutation record");
    require(detail::VolumeAdoptionBuilder::attachment(*owner->snapshot())==metadata,"channel mutation lost metadata");h.submit();
    check(detail::VolumeAdoptionBuilder::prepareImport(*owner,request,stale),"stale publication import prep");
    h.begin();check(ops->prepareClear(1,{},ticket),"advance publication");check(owner->recordAndPublish(h.context(),*ticket),"advance publication record");
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(),*stale)),"stale publication import accepted");h.submit();
    check(detail::VolumeAdoptionBuilder::prepareImport(*owner,request,stale),"stale channel prep");check(owner->setChannel(31,descriptors[0]),"change channel config");
    h.begin();require(NVRHI_FAILED(owner->recordAndPublish(h.context(),*stale)),"stale config import accepted");h.submit();
    // Import replaces the full channel set, including a pending configured channel.
    check(detail::VolumeAdoptionBuilder::prepareImport(*owner,request,ticket),"second import prep");h.begin();
    check(owner->recordAndPublish(h.context(),*ticket),"second import record");auto secondImport=owner->snapshot();
    require(secondImport->bindings().channelMask==31 && detail::VolumeAdoptionBuilder::attachment(*secondImport)==nextMetadata,
        "replacement channels/metadata");auto secondRead=readLater(h,*owner,secondImport);
    auto oldRead=readLater(h,*owner,imported);
    check(owner->host().clear(),"new host epoch");check(owner->prepareGeneration(dense,ticket),"empty repack");
    check(owner->recordAndPublish(h.context(),*ticket),"empty epoch publish");
    require(detail::VolumeAdoptionBuilder::attachment(*owner->snapshot())==nullptr,"clear epoch retained stale metadata");
    ticket.reset();stale.reset();ops.reset();owner.reset();request.attachment=nullptr;foreign.reset();
    h.submit();verify(*secondRead,bytes);verify(*oldRead,original);
    check(detail::VolumeAdoptionBuilder::exportTopology(*imported,after),"frozen export without owner");equalTopology(frozen,after);
    require(detail::VolumeAdoptionBuilder::attachment(*imported)==metadata &&
        detail::VolumeAdoptionBuilder::attachment(*secondImport)==nextMetadata,"retained metadata lifetime");
    std::puts("PASS exact imported raw bytes/non-dense slots, stable host, stale/failure isolation, frozen export and metadata lifetimes");
}
std::vector<char> readArtifact(const std::filesystem::path& path) {
    std::ifstream f(path,std::ios::binary);require(bool(f),"open artifact");
    return std::vector<char>((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
}
}
int main(int argc,const char** argv) {
    donut::log::ConsoleApplicationMode();donut::log::SetCallback([](donut::log::Severity severity,const char* text){
        if(severity>=donut::log::Severity::Warning)++problems;std::fprintf(stderr,"%s\n",text);});
    try {
        {Device device(argc,argv);const auto type=donut::app::GetShaderTypeName(device.device->getGraphicsAPI());
         auto owner=donut::app::GetDirectoryWithExecutable()/"shaders/gvdb_owner"/type/"GVDBAtlasRepack_relocateAtlas.bin";
         auto mutation=donut::app::GetDirectoryWithExecutable()/"shaders/gvdb_voxel"/type/"GVDBAtlasMutation_mutateAtlas.bin";
         for(int i=1;i+1<argc;++i){if(!std::strcmp(argv[i],"--owner-artifact"))owner=argv[i+1];if(!std::strcmp(argv[i],"--mutation-artifact"))mutation=argv[i+1];}
         run(device.device,readArtifact(owner),readArtifact(mutation));}
        require(problems==0,"validation warnings/errors");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}

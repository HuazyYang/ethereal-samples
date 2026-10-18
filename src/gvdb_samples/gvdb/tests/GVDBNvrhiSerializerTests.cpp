// SPDX-License-Identifier: Apache-2.0
// Public serializer fixture: no concrete owner/internal headers.
#include "GVDBNvrhiSerializer.h"
#include "GVDBNvrhiVoxelApi.h"
#include "GVDBNvrhiForeignVolume.h"
#include "../../tests/PortableFixtures.h"
#include "../slang/GVDBPortableVbx.h"
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <nvrhi/core/foundation.h>
#include <nvrhi/core/datablob.h>
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
namespace s = gvdb::slang;
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
s::VbxRawVolume rawFixture(uint8_t seed = 37) {
    auto source = s::fixtures::twoBrickVolume();
    source.volume.atlasResolutionAndApron = {18,12,6,2};
    source.volume.atlasBrickCountAndWidth = {3,2,1,6};
    source.volume.boundsMin = {-2,0,0,0};
    source.volume.boundsMax = {2,2,2,0};
    source.nodes[1] = uint32_t(-2);
    source.nodes[17] = 0;
    source.nodes[33] = uint32_t(-2);
    source.nodes[36] = source.nodes[37] = source.nodes[38] = UINT32_MAX;
    // Deliberately reversed, non-dense slots: leaf0 at5, leaf1 at2, four holes.
    source.nodes[4] = 14; source.nodes[5] = 8; source.nodes[6] = 2;
    source.nodes[20] = 14; source.nodes[21] = 2; source.nodes[22] = 2;
    source.atlasMap.assign(6,s::AtlasNode{-1,-1,-1,-1});
    source.atlasMap[5] = {-2,0,0,0}; source.atlasMap[2] = {0,0,0,1};
    s::VbxRawVolume result;
    std::string error;
    require(s::packVolume(s::fixtures::volumeSource(source),result.packed,error),error.c_str());
    result.transform.preTranslation = {-1.25f,2.5f,-7.75f};
    result.transform.angles = {10,20,-30};
    result.transform.scale = {.5f,2,3};
    result.transform.translation = {8,-4,16};
    result.voxelSize = {.25f,.5f,1.5f};
    // Entire name is significant, including bytes after embedded NULs.
    for (size_t i=0;i<result.gridName.size();++i) result.gridName[i]=uint8_t(i*13+seed);
    result.gridName[17]=0;
    for (uint32_t format=0;format<5;++format) {
        s::VbxChannel channel;
        channel.format=s::AtlasFormat(format);
        channel.bytes.resize(18*12*6*s::atlasStride(channel.format));
        for (size_t i=0;i<channel.bytes.size();++i) channel.bytes[i]=uint8_t(i*37+(i>>3)*17+format*53+seed);
        if(format>=2) for(size_t i=0;i+4<=channel.bytes.size();i+=4) {
            uint32_t bits=(i/4)%3==0 ? 0x7fc1a55au : ((i/4)%3==1 ? 0x80000000u : uint32_t(i*19+seed));
            std::memcpy(channel.bytes.data()+i,&bits,4);
        }
        result.channels.push_back(std::move(channel));
    }
    return result;
}
std::vector<uint8_t> serialized(const s::VbxRawVolume& volume) {
    std::vector<uint8_t> result; std::string error;
    require(s::saveRawVBX(volume,result,error),error.c_str());
    return result;
}
void equalRaw(const s::VbxRawVolume& expected,const s::VbxRawVolume& actual) {
    require(expected.gridName==actual.gridName &&
        std::memcmp(&expected.transform,&actual.transform,sizeof(s::VbxTransform))==0 &&
        std::memcmp(&expected.voxelSize,&actual.voxelSize,sizeof(s::VbxFloat3))==0,"preserve all VBX metadata bytes");
    require(expected.channels.size()==actual.channels.size(),"preserve channel count/order");
    require(expected.packed.nodes==actual.packed.nodes && expected.packed.children==actual.packed.children,
        "preserve graph IDs and original slot coordinates");
    require(expected.packed.levels.size()==actual.packed.levels.size() &&
        std::memcmp(expected.packed.levels.data(),actual.packed.levels.data(),expected.packed.levels.size()*sizeof(s::LevelInfo))==0,
        "preserve level metadata");
    require(std::memcmp(&expected.packed.volume,&actual.packed.volume,sizeof(s::VolumeInfo))==0 &&
        expected.packed.atlasMap.size()==actual.packed.atlasMap.size() &&
        std::memcmp(expected.packed.atlasMap.data(),actual.packed.atlasMap.data(),expected.packed.atlasMap.size()*sizeof(s::AtlasNode))==0,
        "preserve atlas dimensions and sparse original map");
    for(size_t i=0;i<expected.channels.size();++i)
        require(expected.channels[i].format==actual.channels[i].format && expected.channels[i].bytes==actual.channels[i].bytes,
            "preserve every logical channel byte including apron/unused/NaN/-0");
}
g::VolumeHandle makeVolume(IDevice* device,const std::vector<char>& artifact) {
    g::VolumeCreateDesc desc;
    desc.device=device; desc.configuration.levelCount=3; desc.configuration.logDimension={2,1,1};
    desc.remapArtifact=artifact.data(); desc.remapArtifactBytes=artifact.size();
    g::VolumeHandle result; check(g::createVolume(desc,&result),"create public volume"); return result;
}
g::SnapshotHandle snapshot(g::IGVDBVolume* owner) {
    g::SnapshotHandle result; check(owner->getSnapshot(&result),"retain public snapshot"); return result;
}
struct Commands {
    IDevice* device;
    CommandListHandle list;
    explicit Commands(IDevice* value):device(value) {check(device->createCommandList({},&list),"create list");}
    g::RecordingContext context() const {return {device,list};}
    void begin() {list->open();}
    void submit() {
        list->close(); const auto id=device->executeCommandList(list);
        require(id || device->getGraphicsAPI()==GraphicsAPI::D3D11,"submit");
        require(device->waitForIdle(),"GPU completion"); device->runGarbageCollection();
    }
};
std::vector<char> artifact(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary); require(bool(stream),"open artifact");
    return std::vector<char>((std::istreambuf_iterator<char>(stream)),std::istreambuf_iterator<char>());
}
AutoPtr<IDataBlob> blob(const std::vector<uint8_t>& bytes) {
    AutoPtr<IDataBlob> result; check(CreateBlob(bytes.size(),&result),"allocate blob");
    if(!bytes.empty()) std::memcpy(result->GetDataPtr(),bytes.data(),bytes.size());
    return result;
}
g::VbxMetadata metadata(const s::VbxRawVolume& input) {
    g::VbxMetadata result;
    auto copy=[](auto& out,const auto& in){out={in.x,in.y,in.z};};
    copy(result.preTranslation,input.transform.preTranslation);copy(result.angles,input.transform.angles);
    copy(result.scale,input.transform.scale);copy(result.translation,input.transform.translation);
    copy(result.voxelSize,input.voxelSize);std::memcpy(result.gridName,input.gridName.data(),256);
    return result;
}
void equalMetadata(const g::VbxMetadata& a,const g::VbxMetadata& b) {
    require(std::memcmp(&a,&b,sizeof(a))==0,"metadata stays attached to its generation");
}
s::VbxRawVolume finish(g::IGVDBSaveTicket* ticket) {
    AutoPtr<IDataBlob> bytes,diagnostic;
    check(ticket->finishAfterCallerCompletion(&bytes,&diagnostic),"finish coherent save after caller wait");
    require(bytes && !diagnostic,"saved blob and cleared diagnostic");
    s::VbxRawVolume result; std::string error;
    require(s::loadRawVBX(bytes->GetDataPtr(),bytes->GetSize(),result,error),error.c_str());
    IDataBlob* reused=reinterpret_cast<IDataBlob*>(uintptr_t(1));
    require(NVRHI_FAILED(ticket->finishAfterCallerCompletion(&reused)) && !reused,"save completion is one-use and clears output");
    return result;
}
g::SerializerHandle serializer(g::IGVDBVolume* owner) {
    g::SerializerCreateDesc desc;desc.volume=owner;
    g::SerializerHandle result;check(g::createSerializer(desc,&result),"create public serializer");return result;
}
g::SaveTicketHandle prepareSave(g::IGVDBSerializer* io,g::IGVDBSnapshot* snap,Commands& commands,const g::VbxMetadata* overrideValue=nullptr) {
    g::SaveTicketHandle result;AutoPtr<IDataBlob> diagnostic;
    check(io->prepareSaveVBX(snap,overrideValue,{},&result,&diagnostic),"prepare coherent save");
    require(!diagnostic,"successful save preparation clears diagnostic");
    IDataBlob* early=reinterpret_cast<IDataBlob*>(uintptr_t(1));
    require(NVRHI_FAILED(result->finishAfterCallerCompletion(&early)) && !early,"unrecorded readback cannot finish");
    check(io->recordSaveReadback(commands.context(),result),"record coherent readback");
    require(NVRHI_FAILED(io->recordSaveReadback(commands.context(),result)),"save records once");return result;
}
void unchanged(g::IGVDBVolume* owner,g::IGVDBSnapshot* before,const g::TopologyRevision& cpu) {
    const auto after=snapshot(owner);const auto a=before->getDesc(),b=after->getDesc();
    require(std::memcmp(&a.volume,&b.volume,sizeof(a.volume))==0 &&
        a.revision.epoch==b.revision.epoch && a.revision.topology==b.revision.topology &&
        a.revision.placement==b.revision.placement && a.activeLeafCount==b.activeLeafCount &&
        a.levelCount==b.levelCount && a.channelMask==b.channelMask && a.reserved==b.reserved,
        "failed preparation changes no published metadata");
    const auto current=owner->getTopologyRevision();
    require(std::memcmp(&cpu,&current,sizeof(cpu))==0,"failed preparation changes no host revision");
    for(uint32_t c=0;c<32;++c) if(a.channelMask&(1u<<c)) {
        g::ChannelSnapshotDesc x,y;check(before->getChannel(c,x),"before channel");check(after->getChannel(c,y),"after channel");
        require(x.raw.buffer==y.raw.buffer && x.revision.resource==y.revision.resource && x.revision.content==y.revision.content,
            "failed preparation replaces no channel");
    }
}
void factoryFailures() {
    g::SerializerCreateDesc desc;
    auto* output=reinterpret_cast<g::IGVDBSerializer*>(uintptr_t(1));
    require(g::gvdbCreateNvrhiSerializer(g::c_GVDBNvrhiApiVersion+1,&desc,&output)==FE_UNSUPPORTED && !output,"factory version");
    output=reinterpret_cast<g::IGVDBSerializer*>(uintptr_t(1));
    require(NVRHI_FAILED(g::createSerializer(desc,&output)) && !output,"factory null volume");
    require(NVRHI_FAILED(g::createSerializer(desc,nullptr)),"factory null output");
    auto foreign=MAKE_RC_OBJ_PTR(gvdb_api_test::ForeignVolume);desc.volume=foreign;
    output=reinterpret_cast<g::IGVDBSerializer*>(uintptr_t(1));
    require(NVRHI_FAILED(g::createSerializer(desc,&output)) && !output,"reject foreign public implementation");
}
void run(IDevice* device,const std::vector<char>& remap,const std::vector<char>& mutation) {
    Commands commands(device);auto owner=makeVolume(device,remap);auto io=serializer(owner);queryIdentity(io.Get());
    require(io->getVolume()==owner.Get(),"serializer retains exact volume identity");
    check(owner->setChannel(31,{g::AtlasFormat::R32_FLOAT,{-7,0,0,0}}),"baseline channel31");
    g::NodeId baselineLeaf;check(owner->activate(24,0,0,baselineLeaf),"baseline topology");check(owner->finishTopology(),"finish baseline");
    g::GenerationDesc generation;generation.placement.minimumBrickCount={1,1,1};
    g::GenerationTicketHandle initial;check(owner->prepareGeneration(generation,&initial),"prepare baseline");
    commands.begin();check(owner->recordAndPublish(commands.context(),initial),"publish baseline");commands.submit();
    auto baseline=snapshot(owner);auto baselineCpu=owner->getTopologyRevision();
    auto input=rawFixture();auto inputBytes=serialized(input);auto inputBlob=blob(inputBytes);
    void* alias=reinterpret_cast<void*>(uintptr_t(1));
    require(io->prepareLoadVBX(inputBlob,{},reinterpret_cast<g::IGVDBGenerationTicket**>(&alias),
        reinterpret_cast<IDataBlob**>(&alias))==FE_INVALID_ARGS && !alias,"aliased load outputs stay null");
    alias=reinterpret_cast<void*>(uintptr_t(1));
    require(io->prepareSaveVBX(baseline,nullptr,{},reinterpret_cast<g::IGVDBSaveTicket**>(&alias),
        reinterpret_cast<IDataBlob**>(&alias))==FE_INVALID_ARGS && !alias,"aliased save outputs stay null");
    g::SaveTicketHandle unsupported;
    require(io->prepareSaveVBX(baseline,nullptr,{},&unsupported)==FE_UNSUPPORTED && !unsupported,"holey channel mask is explicit unsupported");
    const auto rejectsLoad=[&](IDataBlob* data,const g::SerializationBudget& budget,FRESULT expected=FE_INVALID_ARGS) {
        auto* out=reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));AutoPtr<IDataBlob> diagnostic;
        const auto status=io->prepareLoadVBX(data,budget,&out,&diagnostic);
        require(status==expected && !out,"invalid/budget load clears ticket and returns classified error");
        require(diagnostic && diagnostic->GetSize(),"load failure supplies diagnostic");unchanged(owner,baseline,baselineCpu);
    };
    rejectsLoad(nullptr,{});
    auto bad=inputBytes;bad.pop_back();rejectsLoad(blob(bad),{});
    bad=inputBytes;bad[0]=255;rejectsLoad(blob(bad),{});
    bad=inputBytes;bad[320]=1;rejectsLoad(blob(bad),{}); // compressed file layout rejected by parser
    bad=inputBytes;bad[50]=2;rejectsLoad(blob(bad),{}); // multi-grid rejected
    g::SerializationBudget budget;budget.maxBlobBytes=inputBytes.size()-1;rejectsLoad(inputBlob,budget,FE_OUT_OF_MEMORY);
    budget={};budget.maxAllocatedBytes=0;rejectsLoad(inputBlob,budget,FE_OUT_OF_MEMORY);
    budget={};budget.maxBuffers=1;rejectsLoad(inputBlob,budget,FE_OUT_OF_MEMORY);
    budget={};budget.reserved=1;rejectsLoad(inputBlob,budget);
    auto tooMany=input;tooMany.channels.assign(33,input.channels[0]);
    rejectsLoad(blob(serialized(tooMany)),{},FE_UNSUPPORTED);
    // Target changes after preparation invalidate the replacement without publishing it.
    g::GenerationTicketHandle stale;check(io->prepareLoadVBX(inputBlob,{},&stale),"prepare stale import");
    unchanged(owner,baseline,baselineCpu);check(owner->finishTopology(),"advance target CPU revision");baselineCpu=owner->getTopologyRevision();
    commands.begin();require(NVRHI_FAILED(owner->recordAndPublish(commands.context(),stale)),"reject stale candidate host");commands.submit();
    unchanged(owner,baseline,baselineCpu);
    g::GenerationTicketHandle loaded;check(io->prepareLoadVBX(inputBlob,{},&loaded),"prepare exact import");
    unchanged(owner,baseline,baselineCpu);
    // Preparation must not retain borrowed input bytes.
    std::memset(inputBlob->GetDataPtr(),0,inputBlob->GetSize());inputBlob=nullptr;
    commands.begin();check(owner->recordAndPublish(commands.context(),loaded),"publish exact import");
    require(NVRHI_FAILED(owner->recordAndPublish(commands.context(),loaded)),"import ticket one-use");
    auto a=snapshot(owner);require(a->getDesc().channelMask==31 && owner->getActiveLeafCount()==2 &&
        owner->getConfiguration().logDimension[0]==1 && owner->getTopologyRevision().epoch>baselineCpu.epoch,"atomically replace host configuration and entire channel set");
    g::ChannelSnapshotDesc removed;require(owner->getActiveLeafCount()==2 && a->getChannel(31,removed)==FE_NOT_FOUND,"old channel removed");
    require(owner->isActive({-2,0,0}) && owner->isActive({0,0,0}) && !owner->isAllocated({24,0,0}),"host topology adopted");
    g::VbxMetadata actual;check(io->getMetadata(a,actual),"read imported metadata");equalMetadata(metadata(input),actual);
    auto saveA=prepareSave(io,a,commands);queryIdentity(saveA.Get());
    alias=reinterpret_cast<void*>(uintptr_t(1));
    require(saveA->finishAfterCallerCompletion(reinterpret_cast<IDataBlob**>(&alias),
        reinterpret_cast<IDataBlob**>(&alias))==FE_INVALID_ARGS && !alias,"aliased finish outputs stay null without mapping");
    // Save budget is checked before readback allocation and does not mutate publication.
    budget={};budget.maxBlobBytes=inputBytes.size()-1;
    require(io->prepareSaveVBX(a,nullptr,budget,&unsupported)==FE_OUT_OF_MEMORY && !unsupported,"exact serialized output budget");
    budget={};budget.maxAllocatedBytes=0;
    require(io->prepareSaveVBX(a,nullptr,budget,&unsupported)==FE_OUT_OF_MEMORY && !unsupported,"readback allocation budget");
    auto invalidMetadata=metadata(input);invalidMetadata.voxelSize.x=std::numeric_limits<float>::quiet_NaN();
    require(NVRHI_FAILED(io->prepareSaveVBX(a,&invalidMetadata,{},&unsupported)) && !unsupported,"reject invalid metadata override");
    auto otherOwner=makeVolume(device,remap);auto otherIo=serializer(otherOwner);
    g::SaveTicketHandle wrong;check(io->prepareSaveVBX(a,nullptr,{},&wrong),"prepare foreign-owner attempt");
    require(NVRHI_FAILED(otherIo->recordSaveReadback(commands.context(),wrong)),"foreign serializer rejects unconsumed save ticket");
    check(io->recordSaveReadback(commands.context(),wrong),"rejected foreign attempt leaves ticket usable");
    actual=metadata(input);require(NVRHI_FAILED(otherIo->getMetadata(a,actual)),"foreign owner rejects snapshot metadata");
    equalMetadata(actual,g::VbxMetadata{});
    require(NVRHI_FAILED(otherIo->prepareSaveVBX(a,nullptr,{},&unsupported)) && !unsupported,"foreign owner rejects snapshot save");
    g::GenerationTicketHandle loadB;auto inputB=rawFixture(61);inputB.transform.translation={17,18,19};
    check(io->prepareLoadVBX(blob(serialized(inputB)),{},&loadB),"prepare second input");
    check(owner->recordAndPublish(commands.context(),loadB),"publish second input");auto b=snapshot(owner);
    check(io->getMetadata(a,actual),"old metadata after new import");equalMetadata(metadata(input),actual);
    check(io->getMetadata(b,actual),"new metadata after new import");equalMetadata(metadata(inputB),actual);
    auto saveOld=prepareSave(io,a,commands),saveB=prepareSave(io,b,commands);
    g::VoxelOpsCreateDesc voxelDesc;voxelDesc.volume=owner;voxelDesc.artifact=mutation.data();voxelDesc.artifactBytes=mutation.size();
    g::VoxelOpsHandle ops;check(g::createVoxelOps(voxelDesc,&ops),"operator after import");
    g::GenerationTicketHandle fill;check(ops->prepareFill(2,{3.5f,0,0,0},{},&fill),"prepare GPU edit after import");
    check(owner->recordAndPublish(commands.context(),fill),"publish GPU edit");auto edited=snapshot(owner);
    check(io->getMetadata(edited,actual),"metadata through channel-only mutation");equalMetadata(metadata(inputB),actual);
    auto expectedEdited=inputB;auto& editedBytes=expectedEdited.channels[2].bytes;
    std::fill(editedBytes.begin(),editedBytes.end(),0);const float value=3.5f;
    for(uint32_t z=0;z<6;++z) for(uint32_t y=0;y<12;++y) for(uint32_t x=0;x<18;++x)
        if(x/6==2) std::memcpy(editedBytes.data()+((size_t(z)*12+y)*18+x)*4,&value,4);
    auto saveEdited=prepareSave(io,edited,commands);
    auto overrideValue=metadata(inputB);overrideValue.translation={-100,200,300};
    auto expectedOverride=expectedEdited;expectedOverride.transform.translation={-100,200,300};
    auto saveOverride=prepareSave(io,edited,commands,&overrideValue);overrideValue.translation={0,0,0};
    check(io->getMetadata(edited,actual),"override doesn't change snapshot");equalMetadata(metadata(inputB),actual);
    // Default metadata remains separate from imported generations after explicit host clear.
    check(owner->clearTopology(),"clear imported host");check(owner->finishTopology(),"finish cleared host");
    g::GenerationTicketHandle clear;check(owner->prepareGeneration(generation,&clear),"prepare empty volume");
    check(owner->recordAndPublish(commands.context(),clear),"publish empty volume");auto empty=snapshot(owner);
    check(io->getMetadata(empty,actual),"default metadata after new host epoch");equalMetadata(g::VbxMetadata{},actual);
    require(io->prepareSaveVBX(empty,nullptr,{},&unsupported)==FE_UNSUPPORTED && !unsupported,"empty VBX unsupported explicitly");
    // Every saved object keeps the matching frozen topology, metadata and channel resources.
    baseline=nullptr;a=nullptr;b=nullptr;edited=nullptr;empty=nullptr;
    initial=nullptr;stale=nullptr;loaded=nullptr;loadB=nullptr;fill=nullptr;clear=nullptr;
    ops=nullptr;io=nullptr;owner=nullptr;otherIo=nullptr;otherOwner=nullptr;
    commands.submit();
    equalRaw(input,finish(saveA));equalRaw(input,finish(wrong));equalRaw(input,finish(saveOld));
    equalRaw(inputB,finish(saveB));equalRaw(expectedEdited,finish(saveEdited));equalRaw(expectedOverride,finish(saveOverride));
    std::puts("PASS public VBX five-format sparse slots, exact bytes/metadata, GPU edit, frozen saves, budgets/failures and owner-free completion");
}
void realAsset(IDevice* device,const std::vector<char>& remap,const std::filesystem::path& path) {
    const auto bytes=artifact(path);std::vector<uint8_t> input(bytes.begin(),bytes.end());
    s::VbxRawVolume expected;std::string error;
    require(s::loadRawVBX(input.data(),input.size(),expected,error),error.c_str());
    auto owner=makeVolume(device,remap);auto io=serializer(owner);Commands commands(device);
    check(owner->finishTopology(),"finish configured empty target");g::GenerationTicketHandle load;
    check(io->prepareLoadVBX(blob(input),{},&load),"prepare real explosion import");commands.begin();
    check(owner->recordAndPublish(commands.context(),load),"publish explosion");auto current=snapshot(owner);
    auto save=prepareSave(io,current,commands);current=nullptr;load=nullptr;io=nullptr;owner=nullptr;
    commands.submit();auto actual=finish(save);
    require(expected.channels.size()==actual.channels.size(),"real asset channel count");
    for(size_t c=0;c<expected.channels.size();++c)require(expected.channels[c].format==actual.channels[c].format &&
        expected.channels[c].bytes==actual.channels[c].bytes,"real asset every logical byte");
    equalMetadata(metadata(expected),metadata(actual));
    require(expected.packed.atlasMap.size()==actual.packed.atlasMap.size(),"real asset slot count");
    for(size_t slot=0;slot<expected.packed.atlasMap.size();++slot) {
        const auto x=expected.packed.atlasMap[slot],y=actual.packed.atlasMap[slot];
        require(x.leaf==y.leaf && (x.leaf<0 || (x.x==y.x&&x.y==y.y&&x.z==y.z)),"real asset original leaf/slot association");
    }
    std::puts("PASS real explosion coherent GPU load/save with exact channels and original slots");
}
} // namespace
int main(int argc,const char** argv) {
    donut::log::ConsoleApplicationMode();
    donut::log::SetCallback([](donut::log::Severity severity,const char* text){if(severity>=donut::log::Severity::Warning)++problems;std::fprintf(stderr,"%s\n",text);});
    try {
        factoryFailures();
        {
            Device device(argc,argv);const auto root=donut::app::GetDirectoryWithExecutable()/"shaders";
            const auto type=donut::app::GetShaderTypeName(device.device->getGraphicsAPI());
            auto remap=root/"gvdb_owner"/type/"GVDBAtlasRepack_relocateAtlas.bin";
            auto mutation=root/"gvdb_voxel"/type/"GVDBAtlasMutation_mutateAtlas.bin";
            std::filesystem::path asset;
            for(int i=1;i+1<argc;++i){if(!std::strcmp(argv[i],"--artifact"))remap=argv[i+1];if(!std::strcmp(argv[i],"--operators"))mutation=argv[i+1];if(!std::strcmp(argv[i],"--asset"))asset=argv[i+1];}
            const auto ownerArtifact=artifact(remap);
            if(asset.empty())run(device.device,ownerArtifact,artifact(mutation));else realAsset(device.device,ownerArtifact,asset);
        }
        require(problems==0,"native/NVRHI warnings or errors");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}

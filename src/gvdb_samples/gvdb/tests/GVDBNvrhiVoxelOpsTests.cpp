// SPDX-License-Identifier: Apache-2.0
// Test-only device harness. The public owner has no Donut or native SDK includes.
#include "GVDBNvrhiVoxelOps.h"
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

using Bytes = std::vector<uint8_t>;
using Channels = std::array<Bytes, g::c_MaxChannels>;
constexpr uint32_t allChannels = 0x800003ffu;
const g::ChannelDesc formats[] = {
    {g::AtlasFormat::R8_UINT,{23,0,0,0}},
    {g::AtlasFormat::RGBA8_UINT,{153,51,204,117}},
    {g::AtlasFormat::R32_FLOAT,{-3.5f,0,0,0}},
    {g::AtlasFormat::RGB32_FLOAT,{-1.25f,2.5f,7.75f,0}},
    {g::AtlasFormat::RGBA32_FLOAT,{3.25f,-4.5f,1.75f,9.5f}}
};
Bytes storageValue(g::AtlasFormat format, g::Float4 value) {
    Bytes result(gvdb::slang::atlasStride(format));
    if (format == g::AtlasFormat::R8_UINT) result[0] = uint8_t(value.x);
    else if (format == g::AtlasFormat::RGBA8_UINT) {
        result[0]=uint8_t(value.x); result[1]=uint8_t(value.y); result[2]=uint8_t(value.z); result[3]=uint8_t(value.w);
    } else std::memcpy(result.data(), &value, result.size());
    return result;
}
std::vector<gvdb::slang::AtlasNode> atlasMap(Harness& h, const g::SnapshotBindings& b) {
    const auto bytes = h.read(b.atlasMap);
    std::vector<gvdb::slang::AtlasNode> result(bytes.size()/16);
    if (!bytes.empty()) std::memcpy(result.data(),bytes.data(),bytes.size());
    return result;
}
size_t index(uint32_t x,uint32_t y,uint32_t z,gvdb::slang::UInt4 r) { return (size_t(z)*r.y+y)*r.x+x; }
Bytes fillExpected(const g::SnapshotBindings& b,const std::vector<gvdb::slang::AtlasNode>& map,uint32_t c,const Bytes& value) {
    Bytes out(size_t(b.channels[c].storage.logicalBytes),0);
    const auto r=b.volume.atlasResolutionAndApron,grid=b.volume.atlasBrickCountAndWidth;
    for(uint32_t z=0;z<r.z;++z) for(uint32_t y=0;y<r.y;++y) for(uint32_t x=0;x<r.x;++x) {
        const auto& leaf=map[((z/grid.w)*grid.y+y/grid.w)*grid.x+x/grid.w];
        if(leaf.leaf>=0) std::memcpy(out.data()+index(x,y,z,r)*value.size(),value.data(),value.size());
    }
    return out;
}
Bytes apronExpected(const g::SnapshotBindings& b,const std::vector<gvdb::slang::AtlasNode>& map,uint32_t c,const Bytes& source) {
    const auto r=b.volume.atlasResolutionAndApron,grid=b.volume.atlasBrickCountAndWidth;
    const auto& ch=b.channels[c]; const uint32_t stride=ch.storage.voxelStride,leafWidth=grid.w-2*r.w;
    Bytes out(source.size(),0); const Bytes outside=storageValue(ch.desc.format,ch.desc.voidValue);
    for(uint32_t z=0;z<r.z;++z) for(uint32_t y=0;y<r.y;++y) for(uint32_t x=0;x<r.x;++x) {
        const auto& leaf=map[((z/grid.w)*grid.y+y/grid.w)*grid.x+x/grid.w];
        if(leaf.leaf<0) continue;
        const int px=leaf.x+int(x%grid.w)-int(r.w),py=leaf.y+int(y%grid.w)-int(r.w),pz=leaf.z+int(z%grid.w)-int(r.w);
        const uint8_t* expected=outside.data();
        for(size_t slot=0;slot<map.size();++slot) {
            const auto& neighbor=map[slot]; if(neighbor.leaf<0) continue;
            if(px>=neighbor.x && px<neighbor.x+int(leafWidth) && py>=neighbor.y && py<neighbor.y+int(leafWidth) &&
               pz>=neighbor.z && pz<neighbor.z+int(leafWidth)) {
                const uint32_t sx=uint32_t(slot%grid.x)*grid.w+r.w+uint32_t(px-neighbor.x);
                const uint32_t sy=uint32_t((slot/grid.x)%grid.y)*grid.w+r.w+uint32_t(py-neighbor.y);
                const uint32_t sz=uint32_t(slot/(grid.x*grid.y))*grid.w+r.w+uint32_t(pz-neighbor.z);
                expected=source.data()+index(sx,sy,sz,r)*stride; break;
            }
        }
        std::memcpy(out.data()+index(x,y,z,r)*stride,expected,stride);
    }
    return out;
}
std::unique_ptr<g::PreparedReadback> readLater(Harness& h,g::NvrhiVolume& owner,std::shared_ptr<const g::VolumeGeneration> snapshot) {
    std::unique_ptr<g::PreparedReadback> ticket;
    check(owner.prepareReadback(snapshot,snapshot->bindings().channelMask,ticket),"prepare readback");
    check(owner.recordReadback(h.context(),*ticket),"record readback"); return ticket;
}
void verify(g::PreparedReadback& ticket,const Channels& expected,uint32_t mask=allChannels) {
    g::ChannelReadback output;
    check(g::NvrhiVolume::finishReadbackAfterCompletion(ticket,output),"complete readback");
    require(output.channelMask==mask,"readback channel mask");
    for(uint32_t c=0;c<32;++c) if(mask&(1u<<c)) {
        if(output.channels[c]!=expected[c]) {
            size_t first=0; while(first<std::min(output.channels[c].size(),expected[c].size()) && output.channels[c][first]==expected[c][first]) ++first;
            std::fprintf(stderr,"channel%u first mismatch%zu got%u expected%u\n",c,first,
                first<output.channels[c].size()?output.channels[c][first]:999,first<expected[c].size()?expected[c][first]:999);
            throw std::runtime_error("raw mutation differs from independent CPU bytes");
        }
    }
}
void unchanged(const g::SnapshotBindings& before,const g::SnapshotBindings& after,uint32_t changed) {
    require(before.revision.epoch==after.revision.epoch && before.revision.topology==after.revision.topology &&
        before.revision.placement==after.revision.placement && before.nodes.buffer==after.nodes.buffer &&
        before.children.buffer==after.children.buffer && before.atlasMap.buffer==after.atlasMap.buffer &&
        before.levelMetadata.buffer==after.levelMetadata.buffer,"channel mutation replaced topology");
    for(uint32_t c=0;c<32;++c) if(before.channelMask&(1u<<c)) {
        if(changed&(1u<<c)) require(before.channels[c].raw.buffer!=after.channels[c].raw.buffer &&
            before.channels[c].revision.resource<after.channels[c].revision.resource &&
            before.channels[c].revision.content<after.channels[c].revision.content,"changed channel did not advance");
        else require(before.channels[c].raw.buffer==after.channels[c].raw.buffer &&
            before.channels[c].revision.resource==after.channels[c].revision.resource &&
            before.channels[c].revision.content==after.channels[c].revision.content,"untouched channel advanced");
    }
}
void run(IDevice* device,const std::vector<char>& ownerArtifact,const std::vector<char>& mutationArtifact) {
    Harness h(device);
    g::VolumeOwnerDesc create;create.device=device;create.configuration.levelCount=3;
    create.configuration.logDimension[0]=create.configuration.logDimension[1]=create.configuration.logDimension[2]=1;
    create.remapArtifact=ownerArtifact.data();create.remapArtifactBytes=ownerArtifact.size();
    std::unique_ptr<g::NvrhiVolume> owner;check(g::NvrhiVolume::create(create,owner),"create owner");
    std::unique_ptr<g::NvrhiVoxelOps> ops;
    check(g::NvrhiVoxelOps::create({owner.get(),mutationArtifact.data(),mutationArtifact.size()},ops),"create operators before publication");
    const auto* originalOps=ops.get();
    require(NVRHI_FAILED(g::NvrhiVoxelOps::create({owner.get(),ownerArtifact.data(),ownerArtifact.size()},ops)) && ops.get()==originalOps,
        "incompatible artifact must preserve existing operator");
    std::unique_ptr<g::PreparedGeneration> ticket;
    require(NVRHI_FAILED(ops->prepareClear(0,{},ticket)) && !ticket,"unpublished owner rejected");
    for(uint32_t c=0;c<10;++c) check(owner->setChannel(c,formats[c%5]),"set channel");
    check(owner->setChannel(31,formats[2]),"set channel31");
    for(auto position : {g::Int3{-2,0,0},g::Int3{0,0,0},g::Int3{-2,2,0}}) {
        g::NodeId node;check(owner->host().activate(position.x,position.y,position.z,node),"activate signed leaf");
    }
    check(owner->host().finishTopology(),"finish topology");
    g::GenerationRequest request;request.placement.minimumBrickCount={2,2,1};request.placement.apron=2;
    g::VolumeInfo layout;check(owner->describePlacement(request.placement,layout),"describe");
    Channels uploads,expected;
    std::vector<g::ChannelUpload> uploadDescs;
    for(uint32_t c=0;c<32;++c) if(allChannels&(1u<<c)) {
        g::ChannelStorage storage;check(g::describeChannel(c==31?formats[2].format:formats[c%5].format,
            {layout.atlasResolutionAndApron.x,layout.atlasResolutionAndApron.y,layout.atlasResolutionAndApron.z},storage).result,"storage");
        auto& bytes=uploads[c];bytes.resize(size_t(storage.logicalBytes));
        for(size_t j=0;j<bytes.size();++j) bytes[j]=uint8_t(j*37+c*53+(j>>5));
        if(storage.voxelStride>=4) for(size_t j=0;j+4<=bytes.size();j+=4) {
            uint32_t bits=(j/4)%3==0?0x7fc1a55au:((j/4)%3==1?0x80000000u:uint32_t(j*19));
            std::memcpy(bytes.data()+j,&bits,4);
        }
        uploadDescs.push_back({c,bytes.data(),bytes.size()});
    }
    request.uploads=uploadDescs.data();request.uploadCount=uploadDescs.size();
    check(owner->prepareGeneration(request,ticket),"prepare initial");h.begin();check(owner->recordAndPublish(h.context(),*ticket),"publish initial");h.submit();
    auto initial=owner->snapshot(); auto map=atlasMap(h,initial->bindings());
    require(map.size()==4 && map[3].leaf<0,"unused atlas slot fixture");
    for(uint32_t c=0;c<32;++c) if(allChannels&(1u<<c)) expected[c]=apronExpected(initial->bindings(),map,c,uploads[c]);
    h.begin();auto initialRead=readLater(h,*owner,initial);h.submit();verify(*initialRead,expected);
    // Five same-format copies preserve raw payloads, including NaNs and negative zero.
    h.begin();
    for(uint32_t c=0;c<5;++c) {
        auto before=owner->snapshot();check(ops->prepareCopy(c,c+5,{},ticket),"prepare bit copy");
        check(owner->recordAndPublish(h.context(),*ticket),"record bit copy");unchanged(before->bindings(),owner->snapshot()->bindings(),1u<<(c+5));
        expected[c+5]=expected[c];
    }
    auto copyRead=readLater(h,*owner,owner->snapshot());h.submit();verify(*copyRead,expected);
    const g::Float4 fillValues[]={{87.75f,0,0,0},{.2f,.4f,.8f,.125f},{-4.25f,0,0,0},{1.25f,-2.5f,7.75f,9},{3.25f,-4.5f,9.75f,-8}};
    const g::Float4 storedValues[]={{87,0,0,0},{51,102,204,255},{-4.25f,0,0,0},{1.25f,-2.5f,7.75f,0},{3.25f,-4.5f,9.75f,1}};
    h.begin();
    for(uint32_t c=0;c<5;++c) {
        auto before=owner->snapshot();check(ops->prepareFill(c,fillValues[c],{},ticket),"prepare fill");
        check(owner->recordAndPublish(h.context(),*ticket),"record fill");unchanged(before->bindings(),owner->snapshot()->bindings(),1u<<c);
        expected[c]=fillExpected(owner->snapshot()->bindings(),map,c,storageValue(formats[c].format,storedValues[c]));
    }
    auto filled=owner->snapshot();auto fillRead=readLater(h,*owner,filled);
    check(ops->prepareClear(31,{},ticket),"prepare clear");check(owner->recordAndPublish(h.context(),*ticket),"record clear");
    for(uint32_t c=0;c<5;++c) std::fill(expected[c].begin(),expected[c].end(),0);
    auto clearExpected=expected;auto clearRead=readLater(h,*owner,owner->snapshot());
    check(ops->prepareApron(31,{},ticket),"prepare apron");check(owner->recordAndPublish(h.context(),*ticket),"record apron");
    for(uint32_t c=0;c<5;++c) expected[c]=apronExpected(owner->snapshot()->bindings(),map,c,expected[c]);
    auto apronRead=readLater(h,*owner,owner->snapshot());h.submit();
    Channels fillExpectedBytes=clearExpected;
    for(uint32_t c=0;c<5;++c) fillExpectedBytes[c]=fillExpected(filled->bindings(),map,c,storageValue(formats[c].format,storedValues[c]));
    verify(*fillRead,fillExpectedBytes);verify(*clearRead,clearExpected);verify(*apronRead,expected);
    // Legacy sine is atlas-space (including coordinate-zero planes), tested numerically
    // independently from the bitwise copy/apron expectations.
    h.begin();check(ops->prepareFill(31,{1,0,-1,0},{},ticket),"prepare sine fill");
    check(owner->recordAndPublish(h.context(),*ticket),"record sine fill");
    auto sineRead=readLater(h,*owner,owner->snapshot());h.submit();
    g::ChannelReadback sineResult;check(g::NvrhiVolume::finishReadbackAfterCompletion(*sineRead,sineResult),"read sine");
    const auto sineBindings=owner->snapshot()->bindings();
    const auto r=sineBindings.volume.atlasResolutionAndApron,grid=sineBindings.volume.atlasBrickCountAndWidth;
    for(uint32_t z=0;z<r.z;++z)for(uint32_t y=0;y<r.y;++y)for(uint32_t x=0;x<r.x;++x) {
        const auto& leaf=map[((z/grid.w)*grid.y+y/grid.w)*grid.x+x/grid.w];
        const float scale=12.f/(3.141592f*30.f);
        const float reference=leaf.leaf<0?0.f:std::sin(float(x)*scale)+std::sin(float(y)*scale)+std::sin(float(z)*scale);
        float actual;std::memcpy(&actual,sineResult.channels[31].data()+index(x,y,z,r)*4,4);
        require(std::isfinite(actual)&&std::abs(actual-reference)<.000002f,"legacy sine numerical equation");
    }
    expected[31]=sineResult.channels[31];
    h.begin();check(ops->prepareFill(4,{0,0,0,0},{},ticket),"RGBA allzero fill must clear alpha");
    check(owner->recordAndPublish(h.context(),*ticket),"RGBA allzero record");
    std::fill(expected[4].begin(),expected[4].end(),0);auto rgbaZeroRead=readLater(h,*owner,owner->snapshot());
    h.submit();verify(*rgbaZeroRead,expected);
    // No-op tickets remain authenticated, one-use, and leave the exact snapshot unchanged.
    h.begin();auto beforeNoop=owner->snapshot();g::PreparationBudget noAlloc{0,0};
    check(ops->prepareCopy(2,2,noAlloc,ticket),"prepare same channel noop");check(owner->recordAndPublish(h.context(),*ticket),"consume noop");
    require(owner->snapshot()==beforeNoop,"noop published another generation");
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(),*ticket)),"noop reused");
    check(ops->prepareClear(0,noAlloc,ticket),"prepare zero mask noop");check(owner->recordAndPublish(h.context(),*ticket),"consume zero mask");
    require(owner->snapshot()==beforeNoop,"zero mask advanced generation");h.submit();
    check(ops->prepareClear(0,{},ticket),"output preservation base");auto* preserved=ticket.get();
    require(NVRHI_FAILED(ops->prepareCopy(0,2,{},ticket)) && ticket.get()==preserved,"format mismatch preserves output");
    require(NVRHI_FAILED(ops->prepareFill(1,{2,0,0,0},{},ticket)) && ticket.get()==preserved,"non-normalized color rejected");
    require(NVRHI_FAILED(ops->prepareFill(0,{-1,0,0,0},{},ticket)),"out-of-range byte fill rejected");
    require(NVRHI_FAILED(ops->prepareFill(2,{std::numeric_limits<float>::quiet_NaN(),0,0,0},{},ticket)),"NaN fill rejected");
    require(NVRHI_FAILED(ops->prepareFill(32,{1,0,0,0},{},ticket)),"bad channel rejected");
    require(NVRHI_FAILED(ops->prepareClear(1u<<30,{},ticket)),"missing channel rejected");
    require(ops->prepareFill(2,{1,0,0,0},{UINT64_MAX,1},ticket)==FE_OUT_OF_MEMORY && ticket.get()==preserved &&
        owner->snapshot()==beforeNoop,"partial preparation failure changed publication/output");
    std::unique_ptr<g::PreparedGeneration> stale;
    check(ops->prepareClear(0,{},stale),"stale no-op candidate");
    h.begin();check(ops->prepareFill(31,{0,0,0,0},{},ticket),"allzero fill");check(owner->recordAndPublish(h.context(),*ticket),"record allzero");
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(),*stale)),"stale noop accepted");
    std::fill(expected[31].begin(),expected[31].end(),0);auto allZeroRead=readLater(h,*owner,owner->snapshot());h.submit();verify(*allZeroRead,expected);
    // Same operator must acquire the replacement topology/atlas rather than cache old bindings.
    g::NodeId newLeaf;check(owner->host().activate(2,0,0,newLeaf),"grow topology");
    require(NVRHI_FAILED(ops->prepareClear(0,{},ticket)),"dirty host accepted");
    check(owner->host().finishTopology(),"finish growth");
    require(NVRHI_FAILED(ops->prepareClear(0,{},ticket)),"unpublished host revision accepted");
    request.uploads=nullptr;request.uploadCount=0;request.placement.minimumBrickCount={1,2,1};
    check(owner->prepareGeneration(request,ticket),"prepare grown placement");h.begin();check(owner->recordAndPublish(h.context(),*ticket),"publish growth");h.submit();
    auto grown=owner->snapshot();map=atlasMap(h,grown->bindings());
    h.begin();check(ops->prepareClear(allChannels,{},ticket),"operator reused after growth");check(owner->recordAndPublish(h.context(),*ticket),"clear grown");
    for(uint32_t c=0;c<32;++c) if(allChannels&(1u<<c)) expected[c].assign(size_t(grown->bindings().channels[c].storage.logicalBytes),0);
    // Record two more generations, retain readbacks, then release owner/operators before submit.
    auto grownRead=readLater(h,*owner,owner->snapshot());
    check(ops->prepareFill(31,{-2.75f,0,0,0},{},ticket),"highest channel fill");check(owner->recordAndPublish(h.context(),*ticket),"record highest fill");
    expected[31]=fillExpected(owner->snapshot()->bindings(),map,31,storageValue(g::AtlasFormat::R32_FLOAT,{-2.75f,0,0,0}));
    auto finalRead=readLater(h,*owner,owner->snapshot());
    Channels zeroGrown=expected;std::fill(zeroGrown[31].begin(),zeroGrown[31].end(),0);
    ticket.reset();stale.reset();initial.reset();filled.reset();grown.reset();beforeNoop.reset();ops.reset();owner.reset();
    h.submit();verify(*grownRead,zeroGrown);verify(*finalRead,expected);
    // Empty publication and apron-zero paths are genuine no-ops, with no allocations.
    check(g::NvrhiVolume::create(create,owner),"empty owner");check(owner->setChannel(0,formats[0]),"empty channel");
    check(g::NvrhiVoxelOps::create({owner.get(),mutationArtifact.data(),mutationArtifact.size()},ops),"empty ops");
    check(owner->host().finishTopology(),"empty finish");request.placement.apron=0;
    check(owner->prepareGeneration(request,ticket),"empty prepare");h.begin();check(owner->recordAndPublish(h.context(),*ticket),"empty publish");
    auto empty=owner->snapshot();check(ops->prepareFill(0,{15,0,0,0},noAlloc,ticket),"empty fill noop");
    check(owner->recordAndPublish(h.context(),*ticket),"empty noop record");require(owner->snapshot()==empty,"empty noop revision");h.submit();
    check(owner->host().activate(-2,0,0,newLeaf),"no apron leaf");check(owner->host().finishTopology(),"no apron finish");
    check(owner->prepareGeneration(request,ticket),"no apron generation");h.begin();check(owner->recordAndPublish(h.context(),*ticket),"no apron publish");
    auto noApron=owner->snapshot();check(ops->prepareApron(1,noAlloc,ticket),"zero apron noop");
    check(owner->recordAndPublish(h.context(),*ticket),"zero apron record");require(owner->snapshot()==noApron,"zero apron advanced");h.submit();
    std::puts("PASS raw voxel five-format fill/clear/copy/apron, signed boundaries, NaN bits, revisions, noops, repack reuse and lifetimes");
}
std::vector<char> artifact(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary);require(bool(stream),"open artifact");
    return std::vector<char>((std::istreambuf_iterator<char>(stream)),std::istreambuf_iterator<char>());
}
}
int main(int argc,const char** argv) {
    donut::log::ConsoleApplicationMode();
    donut::log::SetCallback([](donut::log::Severity severity,const char* text){if(severity>=donut::log::Severity::Warning)++problems;std::fprintf(stderr,"%s\n",text);});
    try {
        {
            Device device(argc,argv);
            const auto folder=donut::app::GetShaderTypeName(device.device->getGraphicsAPI());
            auto owner=donut::app::GetDirectoryWithExecutable()/"shaders/gvdb_owner"/folder/"GVDBAtlasRepack_relocateAtlas.bin";
            auto mutation=donut::app::GetDirectoryWithExecutable()/"shaders/gvdb_voxel"/folder/"GVDBAtlasMutation_mutateAtlas.bin";
            for(int i=1;i+1<argc;++i) {if(!std::strcmp(argv[i],"--owner-artifact"))owner=argv[i+1];if(!std::strcmp(argv[i],"--mutation-artifact"))mutation=argv[i+1];}
            run(device.device,artifact(owner),artifact(mutation));
        }
        require(problems==0,"validation warnings/errors");return 0;
    } catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}

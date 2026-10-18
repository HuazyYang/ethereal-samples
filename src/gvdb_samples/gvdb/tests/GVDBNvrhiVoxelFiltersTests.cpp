// SPDX-License-Identifier: Apache-2.0
// Test-only device harness. The public owner has no Donut or native SDK includes.
#include "GVDBNvrhiVoxelFilters.h"
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

using Bytes = std::vector<uint8_t>;
using Channels = std::array<Bytes,5>;
const g::ChannelDesc formats[] = {
    {g::AtlasFormat::R8_UINT,{23,0,0,0}}, {g::AtlasFormat::RGBA8_UINT,{153,51,204,117}},
    {g::AtlasFormat::R32_FLOAT,{-3.5f,0,0,0}}, {g::AtlasFormat::RGB32_FLOAT,{-1.25f,2.5f,7.75f,0}},
    {g::AtlasFormat::RGBA32_FLOAT,{3.25f,-4.5f,1.75f,9.5f}}
};
using Map = std::vector<gvdb::slang::AtlasNode>;
size_t index(uint32_t x,uint32_t y,uint32_t z,gvdb::slang::UInt4 r) { return (size_t(z)*r.y+y)*r.x+x; }
float loadFloat(const Bytes& data,size_t offset) {float value;std::memcpy(&value,data.data()+offset,4);return value;}
void storeFloat(Bytes& data,size_t offset,float value) {std::memcpy(data.data()+offset,&value,4);}
Bytes stored(g::AtlasFormat format,g::Float4 value) {
    Bytes result(gvdb::slang::atlasStride(format));
    if(format==g::AtlasFormat::R8_UINT)result[0]=uint8_t(value.x);
    else if(format==g::AtlasFormat::RGBA8_UINT){result[0]=uint8_t(value.x);result[1]=uint8_t(value.y);result[2]=uint8_t(value.z);result[3]=uint8_t(value.w);}
    else std::memcpy(result.data(),&value,result.size());return result;
}
uint32_t hashValue(uint32_t value) {
    value+=value<<10;value^=value>>6;value+=value<<3;value^=value>>11;value+=value<<15;return value;
}
uint32_t floatBits(float value){uint32_t bits;std::memcpy(&bits,&value,4);return bits;}
float randomValue(uint32_t x,uint32_t y,uint32_t z) {
    const uint32_t hash=hashValue(floatBits(float(x))^hashValue(floatBits(float(y)))^hashValue(floatBits(float(z))));
    uint32_t bits=(hash&0x7fffffu)|0x3f800000u;float value;std::memcpy(&value,&bits,4);return value-1.f;
}
// Per-voxel oracle, independent of the shader's word/lane ownership and topology traversal.
Bytes apron(const g::VolumeInfo& v,const Map& map,g::AtlasFormat format,const Bytes& source,g::Float4 boundary) {
    const auto r=v.atlasResolutionAndApron,grid=v.atlasBrickCountAndWidth;const uint32_t stride=gvdb::slang::atlasStride(format),width=grid.w-2*r.w;
    Bytes result(source.size(),0);const auto outside=stored(format,boundary);
    for(uint32_t z=0;z<r.z;++z)for(uint32_t y=0;y<r.y;++y)for(uint32_t x=0;x<r.x;++x){
        const auto& leaf=map[((z/grid.w)*grid.y+y/grid.w)*grid.x+x/grid.w];if(leaf.leaf<0)continue;
        const uint32_t lx=x%grid.w,ly=y%grid.w,lz=z%grid.w;
        const size_t dst=index(x,y,z,r)*stride;
        if(lx>=r.w&&ly>=r.w&&lz>=r.w&&lx<grid.w-r.w&&ly<grid.w-r.w&&lz<grid.w-r.w){
            std::memcpy(result.data()+dst,source.data()+dst,stride);continue;
        }
        const int wx=leaf.x+int(lx)-int(r.w),wy=leaf.y+int(ly)-int(r.w),wz=leaf.z+int(lz)-int(r.w);
        const uint8_t* from=outside.data();
        for(size_t slot=0;slot<map.size();++slot){const auto& other=map[slot];if(other.leaf<0)continue;
            if(wx>=other.x&&wx<other.x+int(width)&&wy>=other.y&&wy<other.y+int(width)&&wz>=other.z&&wz<other.z+int(width)){
                uint32_t sx=uint32_t(slot%grid.x)*grid.w+r.w+uint32_t(wx-other.x);
                uint32_t sy=uint32_t((slot/grid.x)%grid.y)*grid.w+r.w+uint32_t(wy-other.y);
                uint32_t sz=uint32_t(slot/(grid.x*grid.y))*grid.w+r.w+uint32_t(wz-other.z);
                from=source.data()+index(sx,sy,sz,r)*stride;break;
            }
        }
        std::memcpy(result.data()+dst,from,stride);
    }return result;
}
Bytes filterPass(const g::VolumeInfo& v,const Map& map,g::AtlasFormat format,const Bytes& source,const g::VoxelFilterDesc& desc) {
    const auto r=v.atlasResolutionAndApron,grid=v.atlasBrickCountAndWidth;const uint32_t stride=gvdb::slang::atlasStride(format);
    Bytes result(source.size(),0);
    for(uint32_t z=0;z<r.z;++z)for(uint32_t y=0;y<r.y;++y)for(uint32_t x=0;x<r.x;++x){
        const auto& leaf=map[((z/grid.w)*grid.y+y/grid.w)*grid.x+x/grid.w];if(leaf.leaf<0)continue;
        const size_t dst=index(x,y,z,r)*stride;
        std::memcpy(result.data()+dst,source.data()+dst,stride);
        if(x%grid.w<r.w||y%grid.w<r.w||z%grid.w<r.w||x%grid.w>=grid.w-r.w||y%grid.w>=grid.w-r.w||z%grid.w>=grid.w-r.w)continue;
        const auto p=desc.parameters;
        const size_t neighbor[]={dst-stride,dst+stride,dst-size_t(r.x)*stride,dst+size_t(r.x)*stride,
            dst-size_t(r.x)*r.y*stride,dst+size_t(r.x)*r.y*stride};
        if(desc.operation==g::VoxelFilterOp::ExpandByte){
            bool found=false;for(size_t n:neighbor)found|=source[n]==uint8_t(p.x);
            if(source[dst]==0&&found)result[dst]=uint8_t(p.y);
        }else if(desc.operation==g::VoxelFilterOp::ExpandColor){
            int color[3]{};for(uint32_t c=0;c<3;++c){color[c]=int(source[dst+c]*p.x);for(size_t n:neighbor)color[c]+=int(source[n+c]*p.y);}
            const int maximum=std::max(color[0],std::max(color[1],color[2]));
            for(uint32_t c=0;c<3;++c)result[dst+c]=uint8_t(maximum>255?color[c]*255/maximum:color[c]);result[dst+3]=1;
        }else for(uint32_t c=0;c<stride;c+=4){
            float value=loadFloat(source,dst+c);
            switch(desc.operation){
            case g::VoxelFilterOp::Smooth:value*=p.x;for(size_t n:neighbor)value+=loadFloat(source,n+c);value=value/(p.x+6.f)+p.y;break;
            case g::VoxelFilterOp::Grow:if(value!=0)value+=p.x*10.f;if(value<.01f)value=0;break;
            case g::VoxelFilterOp::Noise:if(value>.01f)value+=randomValue(x,y,z)*p.x;break;
            case g::VoxelFilterOp::Cut:if(leaf.x+int(x%grid.w)-int(r.w)<50&&value>0&&value<2)value=.02f;break;
            default:throw std::runtime_error("oracle operation");
            }storeFloat(result,dst+c,value);
        }
    }return result;
}
Bytes expected(const g::VolumeInfo& v,const Map& map,g::AtlasFormat format,Bytes source,const g::VoxelFilterDesc& desc){
    for(uint32_t iteration=0;iteration<desc.iterations;++iteration){
        source=filterPass(v,map,format,source,desc);
        if(desc.updateApron&&v.atlasResolutionAndApron.w)source=apron(v,map,format,source,desc.boundaryValue);
    }return source;
}
std::unique_ptr<g::PreparedReadback> readLater(Harness& h,g::NvrhiVolume& owner,std::shared_ptr<const g::VolumeGeneration> snapshot){
    std::unique_ptr<g::PreparedReadback> ticket;check(owner.prepareReadback(snapshot,31,ticket),"readback prepare");
    check(owner.recordReadback(h.context(),*ticket),"readback record");return ticket;
}
void verify(g::PreparedReadback& ticket,const Channels& reference,int arithmeticChannel,const char* name){
    g::ChannelReadback output;check(g::NvrhiVolume::finishReadbackAfterCompletion(ticket,output),"readback complete");
    require(output.channelMask==31,"readback mask");
    for(uint32_t c=0;c<5;++c){const auto& actual=output.channels[c];require(actual.size()==reference[c].size(),"readback byte count");
        if(int(c)==arithmeticChannel&&c>=2){for(size_t offset=0;offset<actual.size();offset+=4){
            if(std::memcmp(actual.data()+offset,reference[c].data()+offset,4)==0)continue;
            float a=loadFloat(actual,offset),b=loadFloat(reference[c],offset);
            if(!std::isfinite(a)||std::abs(a-b)>3e-5f*std::max(1.f,std::abs(b))){std::fprintf(stderr,"%s channel%u float%zu actual%.9g expected%.9g\n",name,c,offset/4,a,b);throw std::runtime_error("filter numeric oracle");}
        }}else if(actual!=reference[c]){size_t first=0;while(first<actual.size()&&actual[first]==reference[c][first])++first;
            std::fprintf(stderr,"%s channel%u byte%zu actual%u expected%u\n",name,c,first,unsigned(actual[first]),unsigned(reference[c][first]));throw std::runtime_error("filter exact byte oracle");}
    }
}
void unchanged(const g::SnapshotBindings& before,const g::SnapshotBindings& after,uint32_t changed){
    require(before.revision.epoch==after.revision.epoch&&before.revision.topology==after.revision.topology&&before.revision.placement==after.revision.placement&&
        before.nodes.buffer==after.nodes.buffer&&before.children.buffer==after.children.buffer&&before.levelMetadata.buffer==after.levelMetadata.buffer&&before.atlasMap.buffer==after.atlasMap.buffer,
        "filter changed topology");
    for(uint32_t c=0;c<5;++c)if(c==changed){require(before.channels[c].raw.buffer!=after.channels[c].raw.buffer&&
        before.channels[c].revision.resource<after.channels[c].revision.resource&&before.channels[c].revision.content<after.channels[c].revision.content,"selected channel revision");}
    else require(before.channels[c].raw.buffer==after.channels[c].raw.buffer&&before.channels[c].revision.resource==after.channels[c].revision.resource&&
        before.channels[c].revision.content==after.channels[c].revision.content,"unselected channel changed");
}
struct Fixture {
    std::unique_ptr<g::NvrhiVolume> owner;
    std::unique_ptr<g::NvrhiVoxelFilters> filters;
    g::HostVolume candidate;
    std::array<g::detail::ImportedLeafSlot,4> slots;
    Channels bytes;
    g::detail::VolumeImportRequest request;
    std::array<g::detail::ImportedChannel,5> channels;
    g::VolumeInfo volume{};Map map;
    Fixture(IDevice* device,const std::vector<char>& ownerArtifact,const std::vector<char>& artifact,uint32_t apronWidth=2){
        g::VolumeOwnerDesc create;create.device=device;create.remapArtifact=ownerArtifact.data();create.remapArtifactBytes=ownerArtifact.size();
        create.configuration.levelCount=6;for(uint32_t i=0;i<6;++i)create.configuration.logDimension[i]=1;
        check(g::NvrhiVolume::create(create,owner),"create owner");
        check(g::NvrhiVoxelFilters::create({owner.get(),artifact.data(),artifact.size()},filters),"create filters");
        auto* saved=filters.get();require(NVRHI_FAILED(g::NvrhiVoxelFilters::create({owner.get(),ownerArtifact.data(),ownerArtifact.size()},filters))&&filters.get()==saved,
            "incompatible artifact preserves output");
        check(candidate.configure(create.configuration),"candidate config");
        const int origins[]={-2,0,48,50};const uint32_t placements[]={3,0,5,2};
        for(uint32_t i=0;i<4;++i){g::NodeId id;check(candidate.activate(origins[i],-2,-2,id),"candidate leaf");slots[i]={uint32_t(id.index()),placements[i]};}
        g::NodeId inactive;check(candidate.activate(2,-2,-2,inactive),"inactive leaf");check(candidate.setActive(inactive,false),"inactive flag");
        check(candidate.finishTopology(),"finish candidate");
        const uint32_t pitch=2+2*apronWidth;
        volume.atlasResolutionAndApron={pitch*3,pitch*2,pitch,apronWidth};volume.atlasBrickCountAndWidth={3,2,1,pitch};
        map.resize(6);for(auto& entry:map)entry={0,0,0,-1};
        for(uint32_t i=0;i<4;++i)map[placements[i]]={origins[i],-2,-2,int(slots[i].leafIndex)};
        const auto r=volume.atlasResolutionAndApron;
        for(uint32_t c=0;c<5;++c){const uint32_t stride=gvdb::slang::atlasStride(formats[c].format);auto& data=bytes[c];data.resize(size_t(r.x)*r.y*r.z*stride);
            for(uint32_t z=0;z<r.z;++z)for(uint32_t y=0;y<r.y;++y)for(uint32_t x=0;x<r.x;++x){const size_t offset=index(x,y,z,r)*stride;
                const bool interior=x%pitch>=apronWidth&&y%pitch>=apronWidth&&z%pitch>=apronWidth&&x%pitch<pitch-apronWidth&&y%pitch<pitch-apronWidth&&z%pitch<pitch-apronWidth;
                if(c==0)data[offset]=interior?((x+y+z)%3==0?7:0):7;
                else if(c==1){data[offset]=uint8_t(11+x*7+y*3);data[offset+1]=uint8_t(31+y*13+z*5);data[offset+2]=uint8_t(67+x*3+z*17);data[offset+3]=uint8_t(113+x+y+z);}
                else for(uint32_t component=0;component<stride/4;++component){
                    const float values[]={0.f,.005f,.01f,.5f,1.f,2.f,3.f,-1.f};
                    const float value=c==2?(interior?values[(x+y+z)%8]:31.25f):float(int(x)+2*int(y)-int(z))*0.125f+float(component)*1.25f+(interior?0.f:20.f);
                    storeFloat(data,offset+component*4,value);
                }
            }
            channels[c]={c,formats[c],data.data(),data.size()};
        }
        request.candidate=&candidate;request.placement={{3,2,1},apronWidth,slots.data(),slots.size()};request.channels=channels.data();request.channelCount=5;
    }
    void reset(Harness& h){std::unique_ptr<g::PreparedGeneration> ticket;check(g::detail::VolumeAdoptionBuilder::prepareImport(*owner,request,ticket),"stage fixture");
        check(owner->recordAndPublish(h.context(),*ticket),"publish fixture");volume=owner->snapshot()->bindings().volume;}
};
void run(IDevice* device,const std::vector<char>& ownerArtifact,const std::vector<char>& artifact){
    Harness h(device);Fixture f(device,ownerArtifact,artifact);
    std::unique_ptr<g::PreparedGeneration> ticket;
    require(NVRHI_FAILED(f.filters->prepare({}, {},ticket))&&!ticket,"unpublished owner rejected");
    struct Result{std::unique_ptr<g::PreparedReadback> ticket;Channels bytes;int arithmetic;std::string name;};
    std::vector<Result> results;
    const auto runCase=[&](g::VoxelFilterDesc desc,const char* name){
        f.reset(h);const auto before=f.owner->snapshot();
        check(f.filters->prepare(desc,{},ticket),"filter prepare");check(f.owner->recordAndPublish(h.context(),*ticket),"filter record");
        unchanged(before->bindings(),f.owner->snapshot()->bindings(),desc.channel);
        Channels reference=f.bytes;reference[desc.channel]=expected(f.volume,f.map,formats[desc.channel].format,reference[desc.channel],desc);
        results.push_back({readLater(h,*f.owner,f.owner->snapshot()),std::move(reference),int(desc.channel),name});
        results.push_back({readLater(h,*f.owner,before),f.bytes,-1,std::string(name)+" source immutable"});
    };
    h.begin();g::VoxelFilterDesc desc;
    // Analytic first-pass anchor: stale apron31.25 must participate before any update.
    f.reset(h);desc.channel=2;desc.iterations=1;desc.parameters={2,.25f,0,0};desc.boundaryValue={-3,0,0,0};
    const auto noPre=expected(f.volume,f.map,formats[2].format,f.bytes[2],desc);
    const auto pre=expected(f.volume,f.map,formats[2].format,apron(f.volume,f.map,formats[2].format,f.bytes[2],desc.boundaryValue),desc);
    require(noPre!=pre,"stale-apron fixture does not distinguish forbidden preupdate");
    for(uint32_t channel:{2u,3u,4u})for(bool update:{false,true}){
        desc={};desc.channel=channel;desc.iterations=3;desc.parameters={2,.125f,0,0};desc.boundaryValue={-3,4,-5,6};desc.updateApron=update;
        runCase(desc,update?"smooth three passes plus aprons":"smooth stale aprons");
    }
    desc={};desc.channel=2;desc.operation=g::VoxelFilterOp::Grow;desc.iterations=3;desc.parameters={.02f,0,0,0};desc.updateApron=false;runCase(desc,"grow thresholds");
    desc.parameters.x=-.03f;desc.updateApron=true;runCase(desc,"grow negative increment");
    desc.operation=g::VoxelFilterOp::Noise;desc.parameters.x=.75f;desc.updateApron=false;runCase(desc,"atlas hash repeated noise");
    desc.parameters.x=-.5f;desc.updateApron=true;runCase(desc,"negative noise");
    desc.operation=g::VoxelFilterOp::Cut;desc.iterations=1;desc.parameters={9,-8,7,6};desc.updateApron=false;runCase(desc,"signed cut and x50 boundary");
    desc={};desc.operation=g::VoxelFilterOp::ExpandByte;desc.channel=0;desc.iterations=3;desc.parameters={7.75f,9.75f,0,0};desc.boundaryValue={17,0,0,0};
    runCase(desc,"R8 expansion after each apron");desc.updateApron=false;runCase(desc,"R8 expansion stale aprons");
    desc.skipOverAprons=false;runCase(desc,"skip false retains interior domain");
    desc={};desc.operation=g::VoxelFilterOp::ExpandColor;desc.channel=1;desc.iterations=2;desc.parameters={1.25f,.75f,0,0};desc.boundaryValue={13,29,47,91};
    runCase(desc,"RGBA integer normalization alpha1");desc.parameters={-.5f,.25f,0,0};desc.updateApron=false;runCase(desc,"RGBA signed truncation byte wrap");
    desc.parameters={1.5f,-.5f,0,0};runCase(desc,"RGBA negative neighbor weights");
    // A single seed propagates exactly one six-connected voxel per stable iteration.
    // Unlike the stale-apron fixture above, this detects accidental in-place cascades.
    const Bytes originalLabels=f.bytes[0];std::fill(f.bytes[0].begin(),f.bytes[0].end(),0);
    const auto extent=f.volume.atlasResolutionAndApron;const uint32_t pitch=f.volume.atlasBrickCountAndWidth.w;
    f.bytes[0][index(extent.w,pitch+extent.w,extent.w,extent)]=7; // slot3, world(-2,-2,-2)
    desc={};desc.channel=0;desc.operation=g::VoxelFilterOp::ExpandByte;desc.parameters={7,7,0,0};
    const auto oneWave=expected(f.volume,f.map,formats[0].format,f.bytes[0],desc);
    require(oneWave[index(extent.w+1,pitch+extent.w+1,extent.w,extent)]==0,"one-pass diagonal changed");
    runCase(desc,"isolated byte seed one stable step");desc.iterations=3;
    const auto threeWaves=expected(f.volume,f.map,formats[0].format,f.bytes[0],desc);
    require(threeWaves[index(extent.w+1,pitch+extent.w+1,extent.w,extent)]==7&&oneWave!=threeWaves,"byte propagation fixture not distinguishing passes");
    runCase(desc,"isolated byte seed three stable steps");std::copy(originalLabels.begin(),originalLabels.end(),f.bytes[0].begin());
    // Non-arithmetic apron bytes, including raw NaN payload/-0, must remain bit exact.
    const Bytes originalScalar=f.bytes[2];const uint32_t special[]={0x7fc1a55au,0x80000000u};
    std::memcpy(f.bytes[2].data(),special,sizeof(special));
    desc={};desc.channel=2;desc.operation=g::VoxelFilterOp::Grow;desc.parameters.x=.02f;desc.updateApron=false;
    runCase(desc,"pointwise preserves raw NaN and signed-zero aprons");std::copy(originalScalar.begin(),originalScalar.end(),f.bytes[2].begin());
    h.submit();for(auto& result:results)verify(*result.ticket,result.bytes,result.arithmetic,result.name.c_str());results.clear();
    // Failure and no-op paths preserve exact identity and previously prepared output.
    h.begin();f.reset(h);auto before=f.owner->snapshot();desc={};desc.channel=2;desc.iterations=0;
    check(f.filters->prepare(desc,{0,0},ticket),"zero-iteration no allocation");check(f.owner->recordAndPublish(h.context(),*ticket),"consume zero iterations");
    require(f.owner->snapshot()==before&&NVRHI_FAILED(f.owner->recordAndPublish(h.context(),*ticket)),"noop identity/one use");
    check(f.filters->prepare(desc,{},ticket),"preserved ticket");auto* saved=ticket.get();
    const auto reject=[&](g::VoxelFilterDesc bad,FRESULT code=FE_INVALID_ARGS){const auto status=f.filters->prepare(bad,{},ticket);
        require(status==code&&ticket.get()==saved&&f.owner->snapshot()==before,"guard failure changed output/publication or error code");};
    auto bad=desc;bad.channel=32;reject(bad);bad.channel=31;reject(bad,FE_NOT_FOUND);
    bad=desc;bad.operation=g::VoxelFilterOp(99);reject(bad);bad=desc;bad.channel=0;reject(bad,FE_UNSUPPORTED);
    bad=desc;bad.iterations=g::c_MaxVoxelFilterIterations+1;reject(bad,FE_UNSUPPORTED);
    bad=desc;bad.parameters.x=-6;reject(bad);bad=desc;bad.parameters.w=std::numeric_limits<float>::quiet_NaN();reject(bad);
    bad=desc;bad.boundaryValue.y=std::numeric_limits<float>::infinity();reject(bad);
    bad=desc;bad.operation=g::VoxelFilterOp::Grow;bad.parameters.x=std::numeric_limits<float>::max();reject(bad);
    bad=desc;bad.channel=0;bad.operation=g::VoxelFilterOp::ExpandByte;bad.parameters.x=256;reject(bad);
    bad.parameters={7,9,0,0};bad.boundaryValue.x=.5f;reject(bad);
    bad=desc;bad.channel=1;bad.operation=g::VoxelFilterOp::ExpandColor;bad.parameters.x=32769;reject(bad);
    desc.iterations=2;require(f.filters->prepare(desc,{UINT64_MAX,1},ticket)==FE_OUT_OF_MEMORY&&ticket.get()==saved&&f.owner->snapshot()==before,"partial budget failure");
    std::unique_ptr<g::PreparedGeneration> stale;desc.iterations=0;check(f.filters->prepare(desc,{},stale),"stale no-op");
    desc.iterations=2;check(f.filters->prepare(desc,{},ticket),"advanced filter");check(f.owner->recordAndPublish(h.context(),*ticket),"advanced record");
    require(NVRHI_FAILED(f.owner->recordAndPublish(h.context(),*stale)),"stale no-op accepted");
    Channels final=f.bytes;final[2]=expected(f.volume,f.map,formats[2].format,final[2],desc);
    auto finalRead=readLater(h,*f.owner,f.owner->snapshot());auto oldRead=readLater(h,*f.owner,before);
    // Release operators/owner and all preparation tickets before GPU submission.
    ticket.reset();stale.reset();before.reset();f.filters.reset();f.owner.reset();h.submit();verify(*finalRead,final,2,"retained result");verify(*oldRead,f.bytes,-1,"retained old publication");
    // Pointwise apron0 is supported; nonempty stencil apron0 explicitly rejected.
    Fixture noApron(device,ownerArtifact,artifact,0);h.begin();noApron.reset(h);desc={};desc.channel=2;
    require(noApron.filters->prepare(desc,{},ticket)==FE_UNSUPPORTED,"apron0 stencil accepted");desc.iterations=0;
    check(noApron.filters->prepare(desc,{0,0},ticket),"apron0 zero iterations");check(noApron.owner->recordAndPublish(h.context(),*ticket),"apron0 noop");
    desc.operation=g::VoxelFilterOp::Grow;desc.iterations=1;desc.parameters.x=.1f;
    check(noApron.filters->prepare(desc,{},ticket),"apron0 pointwise");check(noApron.owner->recordAndPublish(h.context(),*ticket),"apron0 record");
    auto pointwise=readLater(h,*noApron.owner,noApron.owner->snapshot());Channels pointwiseExpected=noApron.bytes;
    pointwiseExpected[2]=expected(noApron.volume,noApron.map,formats[2].format,pointwiseExpected[2],desc);h.submit();verify(*pointwise,pointwiseExpected,2,"apron0 pointwise");
    // Empty finished topology still checks format/parameters, but needs no GPU allocation.
    check(noApron.owner->host().clear(),"clear host");check(noApron.owner->host().finishTopology(),"empty finish");g::GenerationRequest emptyRequest;
    check(noApron.owner->prepareGeneration(emptyRequest,ticket),"empty prepare");h.begin();check(noApron.owner->recordAndPublish(h.context(),*ticket),"empty publish");
    auto empty=noApron.owner->snapshot();desc={};desc.channel=2;
    check(noApron.filters->prepare(desc,{0,0},ticket),"empty stencil noop");check(noApron.owner->recordAndPublish(h.context(),*ticket),"empty record");
    require(noApron.owner->snapshot()==empty,"empty filter changed publication");h.submit();
    std::puts("PASS owned filters: six equations, five formats, stable multi-pass, stale aprons, signed sparse boundaries, guards, revisions and retained lifetimes");
}
std::vector<char> artifact(const std::filesystem::path& path){std::ifstream stream(path,std::ios::binary);require(bool(stream),"open artifact");return std::vector<char>((std::istreambuf_iterator<char>(stream)),std::istreambuf_iterator<char>());}
}
int main(int argc,const char** argv){
    donut::log::ConsoleApplicationMode();donut::log::SetCallback([](donut::log::Severity severity,const char* text){if(severity>=donut::log::Severity::Warning)++problems;std::fprintf(stderr,"%s\n",text);});
    try{{Device device(argc,argv);const auto folder=donut::app::GetShaderTypeName(device.device->getGraphicsAPI());
        auto owner=donut::app::GetDirectoryWithExecutable()/"shaders/gvdb_owner"/folder/"GVDBAtlasRepack_relocateAtlas.bin";
        auto filters=donut::app::GetDirectoryWithExecutable()/"shaders/gvdb_filters"/folder/"GVDBAtlasFilters_filterAtlas.bin";
        for(int i=1;i+1<argc;++i){if(!std::strcmp(argv[i],"--owner-artifact"))owner=argv[i+1];if(!std::strcmp(argv[i],"--filter-artifact"))filters=argv[i+1];}
        run(device.device,artifact(owner),artifact(filters));}
        require(problems==0,"validation warnings/errors");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}

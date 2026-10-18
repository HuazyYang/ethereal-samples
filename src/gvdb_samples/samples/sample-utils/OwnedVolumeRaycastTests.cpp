// SPDX-License-Identifier: Apache-2.0
// Test-only device harness. The public owner has no Donut or native SDK includes.
#include "OwnedVolumeRaycast.h"
#include "ComputeRuntime.h"
#include <gvdb/GVDBNvrhiSerializer.h>
#include <gvdb/GVDBNvrhiVoxelApi.h>
#include <gvdb/slang/GVDBPortableVbx.h>
#include "../../tests/PortableFixtures.h"
#include <donut/core/math/math.h>
#include <nvrhi/core/datablob.h>
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

namespace v = gvdb::slang;
struct Image {std::vector<v::Float4> color;std::vector<float> depth;};
std::vector<char> bytes(const std::filesystem::path& path){std::ifstream stream(path,std::ios::binary);require(bool(stream),"open input");return std::vector<char>((std::istreambuf_iterator<char>(stream)),std::istreambuf_iterator<char>());}
AutoPtr<IDataBlob> blob(const void* data,size_t size){AutoPtr<IDataBlob> result;check(CreateBlob(size,&result),"blob");if(size)std::memcpy(result->GetDataPtr(),data,size);return result;}
g::VolumeHandle volume(IDevice* device,const std::vector<char>& artifact){g::VolumeCreateDesc desc;desc.device=device;desc.configuration.levelCount=1;desc.configuration.logDimension[0]=1;desc.remapArtifact=artifact.data();desc.remapArtifactBytes=artifact.size();g::VolumeHandle result;check(g::createVolume(desc,&result),"public owner");return result;}
g::SerializerHandle serializer(g::IGVDBVolume* owner){g::SerializerCreateDesc desc;desc.volume=owner;g::SerializerHandle result;check(g::createSerializer(desc,&result),"public serializer");return result;}
g::SnapshotHandle snapshot(g::IGVDBVolume* owner){g::SnapshotHandle result;check(owner->getSnapshot(&result),"snapshot");return result;}
void load(Harness& h,g::IGVDBVolume* owner,g::IGVDBSerializer* io,const void* data,size_t size){
    auto input=blob(data,size);g::GenerationTicketHandle ticket;AutoPtr<IDataBlob> diagnostic;
    const auto status=io->prepareLoadVBX(input,{},&ticket,&diagnostic);
    if(NVRHI_FAILED(status)){if(diagnostic)std::fprintf(stderr,"%s\n",static_cast<const char*>(diagnostic->GetDataPtr()));check(status,"prepare public VBX");}
    input=nullptr;h.begin();check(owner->recordAndPublish(h.context(),ticket),"record public VBX");ticket=nullptr;h.submit();
}
std::vector<uint8_t> encode(const v::VbxRawVolume& input){std::vector<uint8_t> result;std::string error;require(v::saveRawVBX(input,result,error),error.c_str());return result;}
v::VbxRawVolume fixture(bool gradient=false){
    auto source=v::fixtures::twoBrickVolume();source.volume.atlasResolutionAndApron={18,12,6,2};source.volume.atlasBrickCountAndWidth={3,2,1,6};
    source.volume.boundsMin={-2,0,0,0};source.volume.boundsMax={2,2,2,0};source.nodes[1]=uint32_t(-2);source.nodes[17]=0;source.nodes[33]=uint32_t(-2);
    source.nodes[36]=source.nodes[37]=source.nodes[38]=UINT32_MAX;
    source.nodes[4]=14;source.nodes[5]=8;source.nodes[6]=2;source.nodes[20]=14;source.nodes[21]=2;source.nodes[22]=2;
    source.atlasMap.assign(6,v::AtlasNode{-1,-1,-1,-1});source.atlasMap[5]={-2,0,0,0};source.atlasMap[2]={0,0,0,1};
    v::VbxRawVolume result;std::string error;require(v::packVolume(v::fixtures::volumeSource(source),result.packed,error),error.c_str());
    v::VbxChannel scalar;scalar.format=v::AtlasFormat::R32_FLOAT;scalar.bytes.resize(18*12*6*4);
    for(uint32_t z=0;z<6;++z)for(uint32_t y=0;y<12;++y)for(uint32_t x=0;x<18;++x){const auto& leaf=source.atlasMap[(y/6)*3+x/6];
        float value=leaf.leaf<0?41.f:gradient?(float(leaf.x)+float(x%6)-2.f+.5f+2.f)*.25f:.5f;
        std::memcpy(scalar.bytes.data()+((z*12+y)*18+x)*4,&value,4);
    }
    result.channels.push_back(std::move(scalar));result.channels.push_back({v::AtlasFormat::R8_UINT,std::vector<uint8_t>(18*12*6,7)});return result;
}
std::vector<v::Float4> transfer(){return {{0,0,0,0},{1,.5f,.25f,.5f}};}
v::RaycastParameters analytic(){v::RaycastParameters p{};p.image={1,1,2,128};p.rayOrigin={-3,1,1,0};p.direction00={1,0,0,0};p.march={.25f,0,100,1};p.valueRange={0,1,.5f,1};p.light={-10,1,1,.1f};return p;}
Image read(ethereal::slang::ComputeRuntime& runtime,const v::RaycastOutput& result){Image out;out.color.resize(size_t(result.width)*result.height);out.depth.resize(out.color.size());runtime.read(result.color,out.color.data(),out.color.size()*sizeof(v::Float4));runtime.read(result.depth,out.depth.data(),out.depth.size()*4);return out;}
Image render(ethereal::slang::ComputeRuntime& runtime,g::SnapshotHandle source,v::RaycastParameters p,bool surface=false){return read(runtime,gvdb::sample::raycastOwnedVolume(runtime,std::move(source),0,p,transfer(),surface));}
Image typed(ethereal::slang::ComputeRuntime& runtime,const v::VbxRawVolume& input,v::RaycastParameters p,bool surface=false){
    std::vector<float> scalar(input.channels[0].bytes.size()/4);std::memcpy(scalar.data(),input.channels[0].bytes.data(),input.channels[0].bytes.size());
    v::SlangVolume old(runtime,input.packed,scalar.data(),scalar.size());return read(runtime,old.raycast(p,transfer(),surface));
}
void nearValue(float actual,float expected,const char* name,float tolerance=2e-5f){
    if(std::isinf(expected)){require(std::isinf(actual)&&actual>0,name);return;}
    if(!std::isfinite(actual)||std::abs(actual-expected)>tolerance){std::fprintf(stderr,"%s actual%.9g expected%.9g\n",name,actual,expected);throw std::runtime_error(name);}
}
void compare(const Image& a,const Image& b,const char* name){require(a.color.size()==b.color.size(),"image size");double maxColor=0,maxDepth=0;
    for(size_t i=0;i<a.color.size();++i){const float aa[]={a.color[i].x,a.color[i].y,a.color[i].z,a.color[i].w};const float bb[]={b.color[i].x,b.color[i].y,b.color[i].z,b.color[i].w};
        for(uint32_t c=0;c<4;++c){nearValue(aa[c],bb[c],name);maxColor=std::max(maxColor,double(std::abs(aa[c]-bb[c])));}
        nearValue(a.depth[i],b.depth[i],name,2e-4f);if(std::isfinite(a.depth[i]))maxDepth=std::max(maxDepth,double(std::abs(a.depth[i]-b.depth[i])));
    }std::printf("%s max color delta %.9g depth %.9g\n",name,maxColor,maxDepth);
}
v::RaycastParameters assetCamera(const g::VolumeInfo& info){
    const dm::float3 lower(info.boundsMin.x,info.boundsMin.y,info.boundsMin.z),upper(info.boundsMax.x,info.boundsMax.y,info.boundsMax.z),center=(lower+upper)*.5f;
    const float radius=std::max(1.f,dm::length(upper-lower)*.5f);const auto backward=dm::normalize(dm::float3(.296198f,.5f,.813798f)),forward=-backward;
    const auto right=dm::normalize(dm::cross(forward,dm::float3(0,1,0))),up=dm::cross(right,forward);const float horizontal=std::tan(dm::radians(25.f)),vertical=horizontal*64/96;
    const auto vec=[](dm::float3 x){return v::Float4{x.x,x.y,x.z,0};};
    v::RaycastParameters p{};p.image={96,64,2,uint32_t(std::ceil(radius*2))+8};p.rayOrigin=vec(center+backward*radius*2.8f);p.direction00=vec(forward-right*horizontal+up*vertical);
    p.directionDx=vec(right*(2*horizontal/96));p.directionDy=vec(up*(-2*vertical/64));p.march={1,0,radius*6,1.5f};p.valueRange={0,1,.1f,.995f};p.background={.1f,.2f,.4f,1};p.light=vec(center+dm::float3(-radius,radius*2,-radius));p.light.w=.1f;return p;
}
v::VbxRawVolume save(Harness& h,g::IGVDBSerializer* io,g::IGVDBSnapshot* source){
    g::SaveTicketHandle ticket;check(io->prepareSaveVBX(source,nullptr,{},&ticket),"save frozen reference");h.begin();check(io->recordSaveReadback(h.context(),ticket),"save readback");h.submit();
    AutoPtr<IDataBlob> data;check(ticket->finishAfterCallerCompletion(&data),"save complete");v::VbxRawVolume result;std::string error;
    require(v::loadRawVBX(data->GetDataPtr(),data->GetSize(),result,error),error.c_str());return result;
}
void run(IDevice* device,IDevice* foreign,const std::filesystem::path& ownedPath,const std::filesystem::path& typedPath,
    const std::filesystem::path& mutationPath,const std::filesystem::path& asset){
    const auto remap=bytes(ownedPath/"GVDBAtlasRepack_relocateAtlas.bin"),mutation=bytes(mutationPath);
    Harness h(device);auto owner=volume(device,remap);auto io=serializer(owner);ethereal::slang::ComputeRuntime rawRuntime(device,ownedPath),oldRuntime(device,typedPath);
    const auto input=fixture();const auto data=encode(input);load(h,owner,io,data.data(),data.size());auto initial=snapshot(owner);
    const auto p=analytic();const auto image=render(rawRuntime,initial,p);const float opacity=1-std::exp(-1.f);
    nearValue(image.color[0].x,.5f*opacity,"analytic Beer red");nearValue(image.color[0].y,.25f*opacity,"analytic Beer green");nearValue(image.color[0].z,.125f*opacity,"analytic Beer blue");nearValue(image.color[0].w,opacity,"analytic Beer alpha");nearValue(image.depth[0],1,"analytic entry depth");
    compare(image,typed(oldRuntime,input,p),"signed non-dense raw/structured");
    auto miss=p;miss.rayOrigin.y=3;const auto missed=render(rawRuntime,initial,miss);nearValue(missed.color[0].w,0,"sparse miss");nearValue(missed.depth[0],INFINITY,"sparse miss depth");
    const auto reject=[&](auto call){bool threw=false;try{call();}catch(const std::exception&){threw=true;}require(threw,"invalid render accepted");};
    reject([&]{gvdb::sample::raycastOwnedVolume(rawRuntime,{},0,p,transfer());});
    reject([&]{gvdb::sample::raycastOwnedVolume(rawRuntime,initial,1,p,transfer());});
    reject([&]{gvdb::sample::raycastOwnedVolume(rawRuntime,initial,31,p,transfer());});
    auto invalid=p;invalid.direction00.w=2;reject([&]{gvdb::sample::raycastOwnedVolume(rawRuntime,initial,0,invalid,transfer());});
    // These images fit the total byte bound but exceed a portable dispatch axis.
    // Verify the CPU guard specifically, rather than accepting an allocation or
    // validation-layer error as success. The output would otherwise allocate 8MB.
    for (uint32_t axis=0;axis<2;++axis) {
        auto wide=p;if(axis==0)wide.image.x=65535u*8u+1u;else wide.image.y=65535u*8u+1u;
        bool rejected=false;
        try {gvdb::sample::raycastOwnedVolume(rawRuntime,initial,0,wide,transfer());}
        catch(const std::invalid_argument& error) {rejected=std::strcmp(error.what(),"Owned raycast dispatch dimensions")==0;}
        require(rejected,"oversized dispatch was not rejected before allocation");
    }
    auto other=volume(foreign,remap);check(other->setChannel(0,{}),"foreign channel");check(other->finishTopology(),"foreign finish");g::GenerationTicketHandle otherTicket;
    check(other->prepareGeneration({},&otherTicket),"foreign prepare");Harness otherCommands(foreign);otherCommands.begin();check(other->recordAndPublish(otherCommands.context(),otherTicket),"foreign record");otherCommands.submit();
    auto foreignSnapshot=snapshot(other);reject([&]{gvdb::sample::raycastOwnedVolume(rawRuntime,foreignSnapshot,0,p,transfer());});
    // Failed VBX load must not replace the image's retained publication.
    const char invalidVbx[]="bad";auto invalidBlob=blob(invalidVbx,sizeof(invalidVbx));g::GenerationTicketHandle ticket;
    require(NVRHI_FAILED(io->prepareLoadVBX(invalidBlob,{},&ticket))&&!ticket,"malformed VBX accepted");
    compare(image,render(rawRuntime,snapshot(owner),p),"failed load preserves image");
    g::VoxelOpsCreateDesc opsDesc;opsDesc.volume=owner;opsDesc.artifact=mutation.data();opsDesc.artifactBytes=mutation.size();g::VoxelOpsHandle ops;
    check(g::createVoxelOps(opsDesc,&ops),"public fill operators");check(ops->prepareFill(0,{1,0,0,0},{},&ticket),"prepare density fill");h.begin();check(owner->recordAndPublish(h.context(),ticket),"publish density fill");h.submit();
    auto filled=snapshot(owner);g::ChannelSnapshotDesc oldChannel,newChannel;check(initial->getChannel(0,oldChannel),"old raw");check(filled->getChannel(0,newChannel),"new raw");
    require(oldChannel.raw.buffer!=newChannel.raw.buffer&&newChannel.revision.resource>oldChannel.revision.resource,"fill did not replace channel");
    const auto fillImage=render(rawRuntime,filled,p);nearValue(fillImage.color[0].x,1-std::exp(-2.f),"new generation fill visible");compare(image,render(rawRuntime,initial,p),"old generation after fill");
    g::NodeId leaf;check(owner->activate(0,2,0,leaf),"grow host leaf");check(owner->finishTopology(),"finish host grow");g::GenerationDesc repack;repack.placement.minimumBrickCount={1,3,1};repack.placement.apron=1;
    check(owner->prepareGeneration(repack,&ticket),"prepare new placement");h.begin();check(owner->recordAndPublish(h.context(),ticket),"publish new placement");h.submit();auto grown=snapshot(owner);
    require(grown->getDesc().revision.placement>filled->getDesc().revision.placement&&grown->getDesc().volume.boundsMax.y==4,"new placement metadata not exposed");
    const auto grownReference=save(h,io,grown);compare(render(rawRuntime,grown,p),typed(oldRuntime,grownReference,p),"grown repacked raw/structured");compare(fillImage,render(rawRuntime,filled,p),"old filled generation after repack");
    const auto gradient=fixture(true);const auto gradientBytes=encode(gradient);load(h,owner,io,gradientBytes.data(),gradientBytes.size());auto surfaceSnapshot=snapshot(owner);
    const auto surface=render(rawRuntime,surfaceSnapshot,p,true);nearValue(surface.depth[0],3,"analytic signed surface depth",.002f);nearValue(surface.color[0].x,.5f,"analytic surface red");compare(surface,typed(oldRuntime,gradient,p,true),"surface raw/structured");
    // The empty owner uses explicit background output without null shader resources.
    check(owner->clearTopology(),"clear topology");check(owner->finishTopology(),"finish empty");check(owner->prepareGeneration({},&ticket),"prepare empty");h.begin();check(owner->recordAndPublish(h.context(),ticket),"publish empty");h.submit();
    auto empty=snapshot(owner);auto emptyP=p;emptyP.background={.1f,.2f,.3f,.4f};auto emptyImage=render(rawRuntime,empty,emptyP);nearValue(emptyImage.color[0].z,.3f,"empty background");nearValue(emptyImage.depth[0],INFINITY,"empty depth");
    // Record against a retained old snapshot, then release owner, snapshots and all
    // producer interfaces before submission. The renderer has no volume allocation.
    auto retainedOutput=gvdb::sample::raycastOwnedVolume(rawRuntime,initial,0,p,transfer());
    owner=nullptr;io=nullptr;ops=nullptr;ticket=nullptr;initial=nullptr;filled=nullptr;grown=nullptr;surfaceSnapshot=nullptr;empty=nullptr;
    compare(image,read(rawRuntime,retainedOutput),"owner released before render submission");
    // Actual explosion file, preserving the old portable path as a test-only oracle.
    const auto assetBytes=bytes(asset);auto assetOwner=volume(device,remap);auto assetIo=serializer(assetOwner);load(h,assetOwner,assetIo,assetBytes.data(),assetBytes.size());auto assetSnapshot=snapshot(assetOwner);
    v::VbxRawVolume oldAsset;std::string error;require(v::loadRawVBX(assetBytes.data(),assetBytes.size(),oldAsset,error),error.c_str());const auto camera=assetCamera(assetSnapshot->getDesc().volume);
    const auto assetRaw=render(rawRuntime,assetSnapshot,camera),assetTyped=typed(oldRuntime,oldAsset,camera);compare(assetRaw,assetTyped,"explosion raw/structured volume");
    compare(render(rawRuntime,assetSnapshot,camera,true),typed(oldRuntime,oldAsset,camera,true),"explosion raw/structured surface");
    size_t contribution=0;for(const auto& c:assetRaw.color)if(std::abs(c.x-camera.background.x)+std::abs(c.y-camera.background.y)+std::abs(c.z-camera.background.z)>.03f)++contribution;
    require(contribution>0,"explosion comparison was an empty image");std::printf("PASS owned snapshot raycast, analytic/lifecycle/revision guards and explosion %zu contributing pixels\n",contribution);
}
}
int main(int argc,const char** argv){
    donut::log::ConsoleApplicationMode();donut::log::SetCallback([](donut::log::Severity severity,const char* text){if(severity>=donut::log::Severity::Warning)++problems;std::fprintf(stderr,"%s\n",text);});
    try{{Device device(argc,argv),foreign(argc,argv);const auto format=donut::app::GetShaderTypeName(device.device->getGraphicsAPI());const auto root=donut::app::GetDirectoryWithExecutable();
        auto owned=root/"shaders/gvdb_owned"/format,typedPath=root/"shaders/gvdb_slang"/format,mutation=root/"shaders/gvdb_voxel"/format/"GVDBAtlasMutation_mutateAtlas.bin";
        auto asset=std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR)/"explosion.vbx";
        for(int i=1;i+1<argc;++i){if(!std::strcmp(argv[i],"--owned-shaders"))owned=argv[i+1];if(!std::strcmp(argv[i],"--mutation-artifact"))mutation=argv[i+1];if(!std::strcmp(argv[i],"--asset"))asset=argv[i+1];}
        run(device.device,foreign.device,owned,typedPath,mutation,asset);}
        require(problems==0,"native validation warnings/errors");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}

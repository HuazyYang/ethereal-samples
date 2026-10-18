// SPDX-License-Identifier: Apache-2.0
// Test-only device harness. The public owner has no Donut or native SDK includes.
#include "RenderCustom.h"
#include "../sample-utils/OwnedVolumeIO.h"
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


namespace v=gvdb::slang;
struct Image {std::vector<v::Float4> color;std::vector<float> depth;};
std::vector<char> bytes(const std::filesystem::path& path){std::ifstream stream(path,std::ios::binary);require(bool(stream),"open input");return std::vector<char>((std::istreambuf_iterator<char>(stream)),std::istreambuf_iterator<char>());}
Image read(ethereal::slang::ComputeRuntime& runtime,const v::RaycastOutput& result){Image out;out.color.resize(size_t(result.width)*result.height);out.depth.resize(out.color.size());runtime.read(result.color,out.color.data(),out.color.size()*sizeof(v::Float4));runtime.read(result.depth,out.depth.data(),out.depth.size()*4);return out;}
// Test-only old portable path. Its separate structured atlas binding is the only
// difference; production does not construct a SlangVolume or a duplicate atlas.
Image typed(ethereal::slang::ComputeRuntime& runtime,const v::VbxRawVolume& source,v::RaycastParameters p) {
    std::vector<float> atlas(source.channels[0].bytes.size()/4);std::memcpy(atlas.data(),source.channels[0].bytes.data(),source.channels[0].bytes.size());
    v::SlangVolume volume(runtime,source.packed,atlas.data(),atlas.size());p.volume=volume.info();
    v::RaycastOutput output;output.width=p.image.x;output.height=p.image.y;
    output.color=runtime.buffer(size_t(p.image.x)*p.image.y*sizeof(v::Float4),sizeof(v::Float4));
    output.depth=runtime.buffer(size_t(p.image.x)*p.image.y*sizeof(float),sizeof(float));
    runtime.dispatch("RenderCustomTyped","raycast_kernel",&p,sizeof(p),{
        BindingSetItem::StructuredBuffer_SRV(0,volume.atlas()),BindingSetItem::RawBuffer_SRV(1,volume.nodes()),
        BindingSetItem::RawBuffer_SRV(2,volume.children()),BindingSetItem::StructuredBuffer_SRV(3,volume.levels()),
        BindingSetItem::StructuredBuffer_UAV(0,output.color),BindingSetItem::StructuredBuffer_UAV(1,output.depth)},
        p.image.x/8+uint32_t(p.image.x%8!=0),p.image.y/8+uint32_t(p.image.y%8!=0));
    return read(runtime,output);
}
void compare(const Image& a,const Image& b,const char* label) {
    require(a.color.size()==b.color.size()&&a.depth.size()==b.depth.size(),"comparison dimensions");
    for(size_t i=0;i<a.color.size();++i){
        const float ac[]={a.color[i].x,a.color[i].y,a.color[i].z,a.color[i].w};
        const float bc[]={b.color[i].x,b.color[i].y,b.color[i].z,b.color[i].w};
        for(uint32_t c=0;c<4;++c)require(std::isfinite(ac[c])&&ac[c]==bc[c],"raw/structured custom color differs");
        require(!std::isnan(a.depth[i])&&a.depth[i]==b.depth[i],"raw/structured custom depth differs");
    }
    std::printf("%s: exact color/depth equality (%zu pixels)\n",label,a.color.size());
}
v::VbxRawVolume fixture() {
    auto source=v::fixtures::twoBrickVolume();source.volume.atlasResolutionAndApron={18,12,6,2};source.volume.atlasBrickCountAndWidth={3,2,1,6};
    source.volume.boundsMin={-2,0,0,0};source.volume.boundsMax={2,2,2,0};source.nodes[1]=uint32_t(-2);source.nodes[17]=0;source.nodes[33]=uint32_t(-2);
    source.nodes[36]=source.nodes[37]=source.nodes[38]=UINT32_MAX;
    source.nodes[4]=14;source.nodes[5]=8;source.nodes[6]=2;source.nodes[20]=14;source.nodes[21]=2;source.nodes[22]=2;
    source.atlasMap.assign(6,v::AtlasNode{-1,-1,-1,-1});source.atlasMap[5]={-2,0,0,0};source.atlasMap[2]={0,0,0,1};
    v::VbxRawVolume result;std::string error;require(v::packVolume(v::fixtures::volumeSource(source),result.packed,error),error.c_str());
    v::VbxChannel scalar;scalar.format=v::AtlasFormat::R32_FLOAT;scalar.bytes.resize(18*12*6*4);
    for(uint32_t z=0;z<6;++z)for(uint32_t y=0;y<12;++y)for(uint32_t x=0;x<18;++x){const auto& leaf=source.atlasMap[(y/6)*3+x/6];
        const float value=leaf.leaf<0?41.f:(float(leaf.x)+float(x%6)-2.f+.5f+2.f)*.25f;
        std::memcpy(scalar.bytes.data()+((z*12+y)*18+x)*4,&value,4);
    }
    result.channels.push_back(std::move(scalar));return result;
}
g::SnapshotHandle load(IDevice* device,const std::filesystem::path& remap,const v::VbxRawVolume& input) {
    std::vector<uint8_t> data;std::string error;require(v::saveRawVBX(input,data,error),error.c_str());
    return gvdb::sample::loadOwnedVBX(device,remap,data.data(),data.size());
}
g::SnapshotHandle empty(IDevice* device,const std::filesystem::path& remap) {
    const auto artifact=bytes(remap);g::VolumeCreateDesc desc;desc.device=device;desc.configuration.levelCount=1;desc.configuration.logDimension[0]=1;
    desc.remapArtifact=artifact.data();desc.remapArtifactBytes=artifact.size();g::VolumeHandle owner;
    check(g::createVolume(desc,&owner),"create empty owner");check(owner->setChannel(0,{}),"empty channel");check(owner->finishTopology(),"finish empty");
    g::GenerationTicketHandle ticket;check(owner->prepareGeneration({},&ticket),"prepare empty");Harness h(device);h.begin();check(owner->recordAndPublish(h.context(),ticket),"publish empty");h.submit();
    g::SnapshotHandle result;check(owner->getSnapshot(&result),"empty snapshot");return result;
}
v::RaycastParameters camera(const g::VolumeInfo& info) {
    const dm::float3 lower(info.boundsMin.x,info.boundsMin.y,info.boundsMin.z),upper(info.boundsMax.x,info.boundsMax.y,info.boundsMax.z),center=(lower+upper)*.5f;
    const float radius=std::max(1.f,dm::length(upper-lower)*.5f);const auto backward=dm::normalize(dm::float3(.296198f,.5f,.813798f)),forward=-backward;
    const auto right=dm::normalize(dm::cross(forward,dm::float3(0,1,0))),up=dm::cross(right,forward);const float horizontal=std::tan(dm::radians(25.f)),vertical=horizontal*64/96;
    const auto vec=[](dm::float3 x){return v::Float4{x.x,x.y,x.z,0};};
    v::RaycastParameters p{};p.image={96,64,1,uint32_t(std::ceil(radius*2))+8};p.rayOrigin=vec(center+backward*radius*2.8f);p.direction00=vec(forward-right*horizontal+up*vertical);
    p.directionDx=vec(right*(2*horizontal/96));p.directionDy=vec(up*(-2*vertical/64));p.march={1,0,radius*6,1.5f};p.valueRange={0,1,.1f,.995f};p.background={0,0,.1f,1};p.light=vec(center+dm::float3(-radius,radius*2,-radius));p.light.w=.1f;return p;
}
void run(IDevice* device,const std::filesystem::path& shaders,const std::filesystem::path& remap,const std::filesystem::path& asset) {
    ethereal::slang::ComputeRuntime runtime(device,shaders);const auto input=fixture();auto volume=load(device,remap,input);
    auto p=v::fixtures::raycastParameters(v::fixtures::twoBrickVolume());p.rayOrigin={-3,1,1,0};
    bool rejected=false;
    try {gvdb::sample::dispatchOwnedRaycast(runtime,volume,0,p,{{0,0,0,0}},"RenderCustomTyped","raycast_kernel");}
    catch(const std::invalid_argument& error){rejected=std::strcmp(error.what(),"Compute kernel raycast ABI mismatch")==0;}
    require(rejected,"typed custom artifact accepted by raw dispatcher");
    auto raw=read(runtime,renderCustom(runtime,volume,p));auto reference=typed(runtime,input,p);compare(raw,reference,"signed reversed slots");
    require(std::abs(raw.color[0].x-.6f)<.002f&&std::abs(raw.depth[0]-3)<.002f,"independent analytic threshold/diffuse");
    // Release the sole public snapshot after recording and before submission.
    auto output=renderCustom(runtime,volume,p);volume=nullptr;compare(read(runtime,output),reference,"snapshot released before submission");
    auto blank=empty(device,remap);p.background={1,0,0,0};raw=read(runtime,renderCustom(runtime,blank,p));
    require(raw.color[0].x==0&&raw.color[0].y==0&&raw.color[0].z==.1f&&raw.color[0].w==1&&std::isinf(raw.depth[0])&&raw.depth[0]>0,"fixed empty custom background");
    auto assetSnapshot=gvdb::sample::loadOwnedVBX(device,remap,asset);const auto encoded=bytes(asset);v::VbxRawVolume old;std::string error;
    require(v::loadRawVBX(encoded.data(),encoded.size(),old,error),error.c_str());p=camera(assetSnapshot->getDesc().volume);
    raw=read(runtime,renderCustom(runtime,assetSnapshot,p));reference=typed(runtime,old,p);compare(raw,reference,"explosion custom renderer");
    size_t hits=0;for(float depth:raw.depth)if(std::isfinite(depth))++hits;require(hits>0,"explosion comparison empty");
    std::printf("PASS public custom renderer, %zu explosion hits\n",hits);
}
}
int main(int argc,const char** argv) {
    donut::log::ConsoleApplicationMode();donut::log::SetCallback([](donut::log::Severity severity,const char* text){if(severity>=donut::log::Severity::Warning)++problems;std::fprintf(stderr,"%s\n",text);});
    try{{Device device(argc,argv);const auto format=donut::app::GetShaderTypeName(device.device->getGraphicsAPI());const auto root=donut::app::GetDirectoryWithExecutable();
        auto shaders=root/"shaders/render_kernel"/format,remap=root/"shaders/gvdb_owned"/format/"GVDBAtlasRepack_relocateAtlas.bin";
        for(int i=1;i+1<argc;++i){if(!std::strcmp(argv[i],"--shaders"))shaders=argv[i+1];if(!std::strcmp(argv[i],"--remap"))remap=argv[i+1];}
        run(device.device,shaders,remap,std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR)/"explosion.vbx");}
        require(problems==0,"native validation warnings/errors");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}

// Independent voxel/section equations and retained typed viewer comparisons.
// SPDX-License-Identifier: Apache-2.0
#include "OwnedVolumeView.h"
#include "OwnedVolumeIO.h"
#include "VolumeViewer.h"
#include "ExampleDevice.h"
#include <gvdb/slang/GVDBPortableVbx.h>
#include <gvdb/slang/GVDBTopologyBuilder.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
namespace g=gvdb::slang;
namespace p=gvdb::portable;
namespace s=gvdb::sample;
namespace es=ethereal::slang;
namespace {
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void check(nvrhi::FRESULT status,const char* message) {require(NVRHI_SUCCEEDED(status),message);}
void nearValue(float actual,float expected,const char* message) {
    require(std::isfinite(actual)&&std::abs(actual-expected)<2e-5f,message);
}
const std::vector<g::Float4> tf{{1,0,0,.25f},{0,0,1,.75f}};
struct Image {std::vector<g::Float4> color;std::vector<float> depth;};
Image read(es::ComputeRuntime& runtime,const g::RaycastOutput& output) {
    Image image;image.color.resize(size_t(output.width)*output.height);image.depth.resize(image.color.size());
    runtime.read(output.color,image.color.data(),image.color.size()*16);
    runtime.read(output.depth,image.depth.data(),image.depth.size()*4);return image;
}
void equal(const Image& a,const Image& b) {
    require(a.color.size()==b.color.size(),"image extent");
    require(!std::memcmp(a.color.data(),b.color.data(),a.color.size()*16)&&
        !std::memcmp(a.depth.data(),b.depth.data(),a.depth.size()*4),"raw/typed voxel/section viewer mismatch");
}
class TypedScene:public s::VolumeScene {
 public:
    g::VbxRawVolume data;
    std::unique_ptr<g::SlangVolume> typed;
    TypedScene(es::ComputeRuntime& runtime,bool sparse) {
        std::string error;g::TopologyBuildOptions options;options.logDimensions={2,2};
        if(sparse) {
            options.atlasGrid={3,1,1,0};
            require(g::buildTopology({{-8,0,0,0},{0,0,0,0}},options,data.packed,error),error.c_str());
        } else require(g::buildTopology({{0,0,0,0}},options,data.packed,error),error.c_str());
        const auto r=data.packed.volume.atlasResolutionAndApron;
        std::vector<float> scalar(size_t(r.x)*r.y*r.z,-1234);
        const auto level=data.packed.levels[0];
        for(uint32_t i=0;i<level.nodes.z;++i) {
            const auto* node=data.packed.nodes.data()+(level.nodes.x+size_t(i)*level.nodes.y)/4;
            for(int z=-1;z<=4;++z)for(int y=-1;y<=4;++y)for(int x=-1;x<=4;++x) {
                const auto address=(size_t(int(node[6])+z)*r.y+int(node[5])+y)*r.x+int(node[4])+x;
                scalar[address]=sparse?float(y+2)/8.f:.5f;
            }
        }
        data.channels.resize(1);data.channels[0].format=g::AtlasFormat::R32_FLOAT;
        data.channels[0].bytes.resize(scalar.size()*4);
        std::memcpy(data.channels[0].bytes.data(),scalar.data(),scalar.size()*4);
        typed=std::make_unique<g::SlangVolume>(runtime,data.packed,scalar.data(),scalar.size());
    }
    g::SlangVolume& volume() override {return *typed;}
    const g::PackedVolume& packed() const override {return data.packed;}
    std::vector<g::Float4> transfer() const override {return tf;}
};
class OwnedScene:public s::VolumeScene {
 public:
    p::SnapshotHandle snapshot;
    explicit OwnedScene(p::SnapshotHandle value):snapshot(std::move(value)){}
    p::SnapshotHandle ownedSnapshot() const override {return snapshot;}
    std::vector<g::Float4> transfer() const override {return tf;}
    bool topologyOverlay(std::vector<s::VolumeOverlayLine>&) const override {return true;}
};
p::SnapshotHandle load(es::ComputeRuntime& runtime,TypedScene& scene,const std::filesystem::path& remap) {
    std::vector<uint8_t> encoded;std::string error;
    require(g::saveRawVBX(scene.data,encoded,error),error.c_str());runtime.flush();
    return s::loadOwnedVBX(runtime.device(),remap,encoded.data(),encoded.size());
}
p::SnapshotHandle empty(nvrhi::IDevice* device,const std::filesystem::path& remap) {
    std::ifstream file(remap,std::ios::binary);std::vector<char> artifact((std::istreambuf_iterator<char>(file)),{});
    p::VolumeCreateDesc desc;desc.device=device;desc.configuration.levelCount=2;
    desc.configuration.logDimension[0]=desc.configuration.logDimension[1]=2;
    desc.remapArtifact=artifact.data();desc.remapArtifactBytes=artifact.size();
    p::VolumeHandle owner;check(p::createVolume(desc,&owner),"empty owner");
    check(owner->setChannel(0,{}),"empty scalar");check(owner->finishTopology(),"empty topology");
    p::GenerationTicketHandle ticket;check(owner->prepareGeneration({},&ticket),"empty generation");
    nvrhi::CommandListHandle commands;check(device->createCommandList({},&commands),"empty list");
    commands->open();check(owner->recordAndPublish({device,commands},ticket),"empty publication");commands->close();
    device->executeCommandList(commands);p::SnapshotHandle snapshot;check(owner->getSnapshot(&snapshot),"empty snapshot");
    return snapshot;
}
g::RaycastParameters parameters() {
    g::RaycastParameters q{};q.image={1,1,0,128};q.rayOrigin={-1,1.5f,1.5f,0};q.direction00={1,0,0,0};
    q.march={.25f,0,64,1};q.valueRange={0,1,.25f,1};q.background={.1f,.2f,.3f,.4f};q.light={0,0,0,1};return q;
}
void comparisons(es::ComputeRuntime& runtime,const std::filesystem::path& remap,bool sparse) {
    TypedScene typed(runtime,sparse);OwnedScene owned(load(runtime,typed,remap));
    s::VolumeViewOptions options;options.width=17;options.height=13;options.isovalue=.3f;
    options.sectionHeight=.375f;options.yaw=.7f;options.pitch=.3f;
    for(auto mode:{s::VolumeViewMode::Volume,s::VolumeViewMode::Surface,s::VolumeViewMode::Voxel,
                  s::VolumeViewMode::Section,s::VolumeViewMode::Section2D}) {
        options.mode=mode;
        for(bool inset:{false,true}) {
            options.sectionInset=inset;
            auto expected=read(runtime,s::renderVolumeView(runtime,typed,options));
            equal(read(runtime,s::renderVolumeView(runtime,owned,options)),expected);
        }
    }
    auto q=parameters();
    bool rejected=false;
    try {runtime.requireViewKernel("VolumeView","renderVoxel");}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"typed view artifact accepted");rejected=false;
    try {runtime.requireViewKernel("GVDBRawRaycast","raycastVolume");}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"missing view settings accepted");
    if(sparse)return;
    auto image=read(runtime,s::renderOwnedVolumeView(runtime,owned.snapshot,0,q,tf,s::OwnedVolumeViewMode::Voxel,2));
    nearValue(image.depth[0],1,"voxel first boundary");nearValue(image.color[0].x,.5f,"voxel red");nearValue(image.color[0].z,.5f,"voxel blue");
    image=read(runtime,s::renderOwnedVolumeView(runtime,owned.snapshot,0,q,tf,s::OwnedVolumeViewMode::Section2D,2));
    nearValue(image.depth[0],0,"section depth");nearValue(image.color[0].x,.25f,"section premultiplied red");
    nearValue(image.color[0].z,.25f,"section premultiplied blue");nearValue(image.color[0].w,1,"section alpha");
    q.rayOrigin={1.5f,5,1.5f,0};q.direction00={0,-1,0,0};
    image=read(runtime,s::renderOwnedVolumeView(runtime,owned.snapshot,0,q,tf,s::OwnedVolumeViewMode::Section,1.5f));
    nearValue(image.depth[0],3.5f,"plane depth");nearValue(image.color[0].x,.625f,"section overlay red");
    nearValue(image.color[0].z,.375f,"section overlay blue");
    q=parameters();q.image.x=7;q.image.y=5;
    std::vector<g::Float4> seed(35,g::Float4{.125f,.25f,.375f,.5f});std::vector<float> depths(35,17);
    g::RaycastOutput output{runtime.buffer(seed.size()*16,16,seed.data()),runtime.buffer(depths.size()*4,4,depths.data()),7,5};
    rejected=false;
    try {s::drawOwnedVolumeSection(runtime,owned.snapshot,0,q,tf,2,{UINT32_MAX,0,3,2},output);}
    catch(const std::invalid_argument&){rejected=true;}require(rejected,"overflowing rectangle accepted");
    s::drawOwnedVolumeSection(runtime,owned.snapshot,0,q,tf,2,{2,3,3,2},output);image=read(runtime,output);
    for(uint32_t y=0;y<5;++y)for(uint32_t x=0;x<7;++x) {
        const auto i=y*7+x;
        if(x>=2&&x<5&&y>=3) {
            nearValue(image.color[i].x,.25f,"inset red");nearValue(image.color[i].z,.25f,"inset blue");nearValue(image.depth[i],0,"inset depth");
        } else require(!std::memcmp(&image.color[i],&seed[i],16)&&image.depth[i]==17,"inset modified outside rectangle");
    }
    q=parameters();auto retained=s::renderOwnedVolumeView(runtime,owned.snapshot,0,q,tf,s::OwnedVolumeViewMode::Voxel,2);
    owned.snapshot=nullptr;image=read(runtime,retained);nearValue(image.depth[0],1,"released snapshot lifetime");
}
} // namespace
int main(int argc,const char** argv) {
    try {
        es::ExampleDevice device(argc,argv);es::ComputeRuntime runtime(device.get(),device.shaderPath());
        const auto remap=donut::app::GetDirectoryWithExecutable()/"shaders/gvdb_owned"/
            donut::app::GetShaderTypeName(device.get()->getGraphicsAPI())/"GVDBAtlasRepack_relocateAtlas.bin";
        comparisons(runtime,remap,false);comparisons(runtime,remap,true);runtime.flush();
        OwnedScene noLeaves(empty(device.get(),remap));auto q=parameters();
        for(auto mode:{s::OwnedVolumeViewMode::Voxel,s::OwnedVolumeViewMode::Section,s::OwnedVolumeViewMode::Section2D}) {
            auto image=read(runtime,s::renderOwnedVolumeView(runtime,noLeaves.snapshot,0,q,tf,mode,0));
            require(!std::memcmp(&image.color[0],&q.background,16)&&std::isinf(image.depth[0])&&image.depth[0]>0,"empty view result");
        }
        s::VolumeViewOptions options;options.width=9;options.height=7;options.topology=true;options.sectionInset=true;
        for(auto mode:{s::VolumeViewMode::Volume,s::VolumeViewMode::Surface,s::VolumeViewMode::Voxel,s::VolumeViewMode::Section,s::VolumeViewMode::Section2D}) {
            options.mode=mode;auto image=read(runtime,s::renderVolumeView(runtime,noLeaves,options));
            for(size_t i=0;i<image.color.size();++i)require(!std::memcmp(&image.color[i],&options.background,16)&&
                std::isinf(image.depth[i])&&image.depth[i]>0,"empty snapshot-only viewer");
        }
        std::puts("Owned voxel/section/inset raw/typed, analytic, empty and lifetime PASS");return 0;
    } catch(const std::exception& error) {std::fprintf(stderr,"%s\n",error.what());return 1;}
}

// Analytic scalar/color fixtures independent of point fusion and the viewer.
// SPDX-License-Identifier: Apache-2.0
#include "ExampleDevice.h"
#include "VolumeSurface.h"
#include "../gvdb_samples/gvdb/slang/GVDBTopologyBuilder.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
namespace g=gvdb::slang;
namespace es=ethereal::slang;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void expectNear(float actual,float expected,const char* message){
    if(!std::isfinite(actual)||std::abs(actual-expected)>2e-4f){
        std::fprintf(stderr,"%s: actual %.9g expected %.9g\n",message,double(actual),double(expected));
        throw std::runtime_error(message);
    }
}
struct Fixture {
    g::PackedVolume packed;
    std::vector<float> scalar;
    std::vector<uint32_t> color;
    std::unique_ptr<g::SlangVolume> volume;
    nvrhi::BufferHandle colorBuffer;
    g::Int4 shift;
    Fixture(es::ComputeRuntime& runtime,g::Int4 offset,int mode):shift(offset){
        g::TopologyBuildOptions options;options.logDimensions={2,2};options.atlasGrid={2,1,1,0};
        std::string error;
        if(!g::buildTopology({{-8+shift.x,shift.y,shift.z,0},{shift.x+(mode==3?-4:0),shift.y,shift.z,0}},options,packed,error))
            throw std::runtime_error(error);
        const auto res=packed.volume.atlasResolutionAndApron;
        // Deliberately stale zero aprons must not turn missing positive space into iso0.
        scalar.resize(size_t(res.x)*res.y*res.z,0);color.resize(scalar.size(),0xffff00ff);
        const auto& level=packed.levels[0];
        for(uint32_t i=0;i<level.nodes.z;++i){
            const auto* node=packed.nodes.data()+(level.nodes.x+size_t(i)*level.nodes.y)/4;
            for(uint32_t z=0;z<4;++z)for(uint32_t y=0;y<4;++y)for(uint32_t x=0;x<4;++x){
                const auto address=((size_t(node[6])+z)*res.y+node[5]+y)*res.x+node[4]+x;
                const bool first=int32_t(node[1])==shift.x-8;
                scalar[address]=mode==3?float(int32_t(node[1])-shift.x)+float(x)+4.5f:
                    mode==2?0.f:mode==1?2.f:first?float(x)-1.75f:3.f;
                color[address]=(32u+32u*x)|(64u<<8)|(192u<<16)|(255u<<24);
            }
        }
        volume=std::make_unique<g::SlangVolume>(runtime,packed,scalar.data(),scalar.size());
        colorBuffer=runtime.buffer(color.size()*4,0,color.data(),true);
    }
    g::RaycastParameters parameters()const{
        g::RaycastParameters p{};p.image={1,1,0,128};p.march={.25f,0,64,1};p.valueRange={-2,3,0,1};
        p.rayOrigin={float(shift.x)-7,float(shift.y)+1.5f,float(shift.z)+1.5f,0};p.direction00={1,0,0,0};
        p.background={.03f,.07f,.11f,.4f};p.light={float(shift.x)+4,float(shift.y)+1.5f,float(shift.z)+1.5f,0};
        return p;
    }
};
struct Pixel {g::Float4 color;float depth;};
const std::vector<g::Float4> transfer{{.2f,.4f,.6f,1},{.2f,.4f,.6f,1}};
Pixel render(es::ComputeRuntime& runtime,Fixture& fixture,g::RaycastParameters p,nvrhi::IBuffer* color,float background=3){
    const auto image=gvdb::sample::renderVolumeSurface(runtime,*fixture.volume,p,transfer,color,background);
    require(image.width==1&&image.height==1,"surface image dimensions");Pixel pixel{};
    runtime.read(image.color,&pixel.color,sizeof(pixel.color));runtime.read(image.depth,&pixel.depth,sizeof(pixel.depth));
    require(std::isfinite(pixel.color.x)&&std::isfinite(pixel.color.y)&&std::isfinite(pixel.color.z)&&std::isfinite(pixel.color.w),"nonfinite surface color");
    return pixel;
}
void colorNear(const Pixel& pixel,float r,float green,float blue){
    expectNear(pixel.color.x,r,"surface red");expectNear(pixel.color.y,green,"surface green");expectNear(pixel.color.z,blue,"surface blue");expectNear(pixel.color.w,1,"surface alpha");
}
void miss(const Pixel& pixel,const g::RaycastParameters& p){
    require(std::isinf(pixel.depth)&&pixel.depth>0,"sparse hole generated a false surface");
    expectNear(pixel.color.x,p.background.x,"miss red");expectNear(pixel.color.y,p.background.y,"miss green");
    expectNear(pixel.color.z,p.background.z,"miss blue");expectNear(pixel.color.w,p.background.w,"miss alpha");
}
void plane(es::ComputeRuntime& runtime,g::Int4 shift){
    Fixture fixture(runtime,shift,0);auto p=fixture.parameters();
    auto pixel=render(runtime,fixture,p,fixture.colorBuffer);
    expectNear(pixel.depth,1.25f,"inside level-set crossing depth");colorNear(pixel,96.f/255,64.f/255,192.f/255);
    p.rayOrigin.x=float(shift.x)-4.75f;p.direction00={-1,0,0,0};
    pixel=render(runtime,fixture,p,fixture.colorBuffer);expectNear(pixel.depth,1,"reverse crossing depth");colorNear(pixel,96.f/255,64.f/255,192.f/255);
    p.rayOrigin.x=float(shift.x)-5.75f;
    pixel=render(runtime,fixture,p,fixture.colorBuffer);expectNear(pixel.depth,0,"ray starting exactly on level set");
    p=fixture.parameters();p.light={float(shift.x)-10,float(shift.y)+1.5f,float(shift.z)+1.5f,.25f};
    pixel=render(runtime,fixture,p,fixture.colorBuffer);colorNear(pixel,24.f/255,16.f/255,48.f/255);
    p.light.x=float(shift.x)-5.75f;
    pixel=render(runtime,fixture,p,fixture.colorBuffer);colorNear(pixel,24.f/255,16.f/255,48.f/255);
    p=fixture.parameters();pixel=render(runtime,fixture,p,nullptr);colorNear(pixel,.2f,.4f,.6f);
    p.direction00={0,0,0,0};miss(render(runtime,fixture,p,fixture.colorBuffer),p);
    // First pixel is off-axis (forward cosine 3/5). Its surface is at view-Z
    // 1.25 but ray distance 25/12, outside radial far=1.5 and inside plane far=1.5.
    p=fixture.parameters();p.image.x=3;p.rayOrigin.y=float(shift.y)+.5f;
    p.direction00={1,2,0,1};p.directionDx={0,-4.f/3,0,0};p.march.y=.1f;p.march.z=1.5f;p.light.w=1;
    auto offAxis=[&](){
        auto image=gvdb::sample::renderVolumeSurface(runtime,*fixture.volume,p,transfer,fixture.colorBuffer,3);
        Pixel result{};runtime.read(image.color,&result.color,sizeof(result.color));runtime.read(image.depth,&result.depth,4);return result;
    };
    pixel=offAxis();expectNear(pixel.depth,25.f/12,"off-axis view-Z far clip");colorNear(pixel,96.f/255,64.f/255,192.f/255);
    p.march.y=1.3f;miss(offAxis(),p);
    p.march.y=.1f;p.direction00.w=0;miss(offAxis(),p);
    p=fixture.parameters();p.direction00.w=.5f;bool rejected=false;
    try{render(runtime,fixture,p,fixture.colorBuffer);}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"unsupported ray clip mode accepted");
    p=fixture.parameters();rejected=false;
    try{render(runtime,fixture,p,fixture.colorBuffer,std::numeric_limits<float>::quiet_NaN());}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"NaN scalar background accepted");
    auto shortColor=runtime.buffer(4,0,nullptr,true);rejected=false;
    try{render(runtime,fixture,p,shortColor);}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"undersized color atlas accepted");
    auto structured=runtime.buffer(fixture.color.size()*4,4,fixture.color.data());rejected=false;
    try{render(runtime,fixture,p,structured);}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"nonraw color atlas accepted");
}
}
int main(int argc,const char** argv){try{
    es::ExampleDevice device(argc,argv);es::ComputeRuntime runtime(device.get(),device.shaderPath());
    plane(runtime,{0,0,0,0});plane(runtime,{12,-8,4,0});
    Fixture positive(runtime,{0,0,0,0},1);auto p=positive.parameters();p.rayOrigin.x=-10;
    miss(render(runtime,positive,p,positive.colorBuffer),p);
    p.rayOrigin={-2,1.5f,-1,0};p.direction00={0,0,1,0};
    miss(render(runtime,positive,p,positive.colorBuffer),p);
    miss(render(runtime,positive,p,positive.colorBuffer,0),p);
    Fixture adjacent(runtime,{0,0,0,0},3);p=adjacent.parameters();p.rayOrigin.x=-5;p.light.w=1;
    const auto boundary=render(runtime,adjacent,p,adjacent.colorBuffer);
    expectNear(boundary.depth,1,"surface exactly across adjacent sparse leaf interiors");
    colorNear(boundary,32.f/255,64.f/255,192.f/255);
    Fixture flat(runtime,{0,0,0,0},2);p=flat.parameters();p.light=p.rayOrigin;p.light.w=.25f;
    const auto pixel=render(runtime,flat,p,flat.colorBuffer);
    expectNear(pixel.depth,0,"zero-gradient surface depth");colorNear(pixel,16.f/255,16.f/255,48.f/255);
    std::puts("PASS signed/shifted level sets, nearest RGBA8, outward normals, sparse holes, finite gradients and input bounds");
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"Volume surface: %s\n",e.what());return 1;}}

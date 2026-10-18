// Render a VBX scalar volume using the same offline Slang kernels on every API.
// SPDX-License-Identifier: Apache-2.0
#include "ExampleDevice.h"
#include <gvdb/GVDBSlangVolume.h>
#include <gvdb/slang/GVDBPortableVbx.h>
#include <donut/core/math/math.h>
#include <stb_image_write.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
gvdb::slang::Float4 vector4(dm::float3 v,float w=0) {return {v.x,v.y,v.z,w};}
std::vector<gvdb::slang::Float4> transferFunction() {
    using F=gvdb::slang::Float4;
    const F starts[4]={{0,0,0,0},{1,1,0,0.4f},{1,0,0,0.3f},{0.2f,0.2f,0.2f,0.1f}};
    const F ends[4]={{1,1,0,0.1f},{1,0,0,0.3f},{0.2f,0.2f,0.2f,0.1f},{0,0,0,0}};
    std::vector<F> result(256);
    for(unsigned i=0;i<256;++i) {
        const float coordinate=float(i)*4/255;const unsigned segment=std::min(3u,unsigned(coordinate));
        const float t=coordinate-segment;const F a=starts[segment],b=ends[segment];
        result[i]={a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t,a.w+(b.w-a.w)*t};
    }
    return result;
}
}
int main(int argc,const char** argv) {
    try {
        int width=1024,height=768;float step=0.25f;bool surface=false;
        std::filesystem::path input=std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR)/"explosion.vbx",output="out_rendtofile.png";
        for(int i=1;i<argc;++i) {
            const auto argument=[&]()->const char* {if(++i>=argc)throw std::invalid_argument("Missing option value");return argv[i];};
            if(!std::strcmp(argv[i],"--help")) {
                std::puts("render-to-file [-dx11|-dx12|-vk|-cuda] [--adapter N] [--validation]\n"
                    "  [--input volume.vbx] [--output image.png] [--width N] [--height N] [--step N] [--surface]");
                return 0;
            }
            if(!std::strcmp(argv[i],"--input"))input=argument();
            else if(!std::strcmp(argv[i],"--output"))output=argument();
            else if(!std::strcmp(argv[i],"--width"))width=std::stoi(argument());
            else if(!std::strcmp(argv[i],"--height"))height=std::stoi(argument());
            else if(!std::strcmp(argv[i],"--step"))step=std::stof(argument());
            else if(!std::strcmp(argv[i],"--surface"))surface=true;
        }
        if(width<1||height<1||uint64_t(width)*height>16*1024*1024||!std::isfinite(step)||step<=0)
            throw std::invalid_argument("Invalid image dimensions or ray step");
        gvdb::slang::VbxVolume loaded;std::string error;
        if(!gvdb::slang::loadVBXFile(input,loaded,error))throw std::runtime_error(error);
        ethereal::slang::ExampleDevice device(argc,argv);
        ethereal::slang::ComputeRuntime runtime(device.get(),device.shaderPath());
        gvdb::slang::SlangVolume volume(runtime,loaded.packed,loaded.atlas.data(),loaded.atlas.size());
        const auto& info=volume.info();
        const dm::float3 lower(info.boundsMin.x,info.boundsMin.y,info.boundsMin.z);
        const dm::float3 upper(info.boundsMax.x,info.boundsMax.y,info.boundsMax.z);
        const dm::float3 center=(lower+upper)*0.5f;
        // Frame index-space bounds, so differently transformed VBX assets remain visible.
        const float radius=std::max(1.f,dm::length(upper-lower)*0.5f);
        const dm::float3 backwards=dm::normalize(dm::float3(0.296198f,0.5f,0.813798f));
        const dm::float3 forward=-backwards,right=dm::normalize(dm::cross(forward,dm::float3(0,1,0)));
        const dm::float3 up=dm::cross(right,forward);
        const float horizontal=std::tan(dm::radians(25.f)),vertical=horizontal*float(height)/width;
        gvdb::slang::RaycastParameters p{};
        p.image={uint32_t(width),uint32_t(height),256,uint32_t(std::ceil(radius*2/step))+8};
        p.rayOrigin=vector4(center+backwards*radius*2.8f);
        p.direction00=vector4(forward-right*horizontal+up*vertical);
        p.directionDx=vector4(right*(2*horizontal/width));p.directionDy=vector4(up*(-2*vertical/height));
        p.march={step,0,radius*6,1.5f};p.valueRange={0,1,0.1f,0.995f};
        p.background={0.1f,0.2f,0.4f,1};p.light=vector4(center+dm::float3(-radius,radius*2,-radius),0.1f);
        runtime.flush(); // Exclude asset upload from GPU render timing.
        const auto start=std::chrono::steady_clock::now();
        auto rendered=volume.raycast(p,transferFunction(),surface);
        std::vector<gvdb::slang::Float4> color(size_t(width)*height);
        runtime.read(rendered.color,color.data(),color.size()*sizeof(color[0]));
        const double milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        std::vector<uint8_t> rgba(color.size()*4);size_t changed=0;
        for(size_t i=0;i<color.size();++i) {
            const auto& c=color[i];
            if(!std::isfinite(c.x)||!std::isfinite(c.y)||!std::isfinite(c.z)||!std::isfinite(c.w))
                throw std::runtime_error("Renderer produced nonfinite pixels");
            if(std::abs(c.x-p.background.x)+std::abs(c.y-p.background.y)+std::abs(c.z-p.background.z)>0.03f)++changed;
            const float channels[4]={c.x,c.y,c.z,c.w};
            for(unsigned j=0;j<4;++j)rgba[i*4+j]=uint8_t(std::clamp(channels[j],0.f,1.f)*255+0.5f);
        }
        if(!changed)throw std::runtime_error("No volume samples contributed to the image");
        if(!stbi_write_png(output.string().c_str(),width,height,4,rgba.data(),width*4))
            throw std::runtime_error("Cannot write output image");
        std::printf("%u leaves, %zu voxels, %zu non-background pixels; render/readback %.3f ms\n",
            loaded.packed.levels[0].nodes.z,loaded.atlas.size(),changed,milliseconds);
        std::printf("Wrote %s\n",output.string().c_str());return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"render-to-file: %s\n",error.what());return 1;}
}

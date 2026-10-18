// Render a VBX scalar volume using the same offline Slang kernels on every API.
// SPDX-License-Identifier: Apache-2.0
#include "ExampleDevice.h"
#include "../sample-utils/OwnedVolumeRaycast.h"
#include <gvdb/GVDBNvrhiSerializer.h>
#include <nvrhi/core/datablob.h>
#include <fstream>
#include <donut/core/math/math.h>
#include <stb_image_write.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
void check(nvrhi::FRESULT status,const char* operation) {
    if(NVRHI_FAILED(status))throw std::runtime_error(std::string(operation)+": "+std::to_string(status));
}
nvrhi::AutoPtr<nvrhi::IDataBlob> readBlob(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input)throw std::runtime_error("Cannot open "+path.string());
    const auto size=input.tellg();if(size<0||uint64_t(size)>SIZE_MAX)throw std::runtime_error("Invalid file size: "+path.string());
    nvrhi::AutoPtr<nvrhi::IDataBlob> result;check(nvrhi::CreateBlob(size_t(size),&result),"allocate input blob");
    input.seekg(0);if(size&&!input.read(static_cast<char*>(result->GetDataPtr()),size))throw std::runtime_error("Cannot read "+path.string());
    return result;
}
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
        ethereal::slang::ExampleDevice device(argc,argv);
        const auto shaderPath=donut::app::GetDirectoryWithExecutable()/"shaders/gvdb_owned"/
            donut::app::GetShaderTypeName(device.get()->getGraphicsAPI());
        const auto remap=readBlob(shaderPath/"GVDBAtlasRepack_relocateAtlas.bin");
        gvdb::portable::VolumeCreateDesc create;create.device=device.get();create.configuration.levelCount=1;
        create.configuration.logDimension[0]=1;create.remapArtifact=remap->GetDataPtr();create.remapArtifactBytes=remap->GetSize();
        gvdb::portable::VolumeHandle owner;check(gvdb::portable::createVolume(create,&owner),"create volume owner");
        gvdb::portable::SerializerCreateDesc serializerDesc;serializerDesc.volume=owner;
        gvdb::portable::SerializerHandle serializer;check(gvdb::portable::createSerializer(serializerDesc,&serializer),"create serializer");
        gvdb::portable::GenerationTicketHandle load;auto file=readBlob(input);nvrhi::AutoPtr<nvrhi::IDataBlob> diagnostic;
        const auto status=serializer->prepareLoadVBX(file,{},&load,&diagnostic);
        if(NVRHI_FAILED(status))throw std::runtime_error(diagnostic?std::string(static_cast<const char*>(diagnostic->GetDataPtr()),diagnostic->GetSize()):"Cannot load VBX");
        file=nullptr;
        nvrhi::CommandListHandle upload;check(device.get()->createCommandList({},&upload),"create upload list");
        upload->open();check(owner->recordAndPublish({device.get(),upload},load),"record loaded volume");upload->close();
        const auto submission=device.get()->executeCommandList(upload);
        if(!submission&&device.get()->getGraphicsAPI()!=nvrhi::GraphicsAPI::D3D11)throw std::runtime_error("Submit volume upload failed");
        if(!device.get()->waitForIdle())throw std::runtime_error("Volume upload failed");device.get()->runGarbageCollection();
        gvdb::portable::SnapshotHandle volume;check(owner->getSnapshot(&volume),"retain volume snapshot");
        const auto snapshot=volume->getDesc();gvdb::portable::ChannelSnapshotDesc scalar;
        check(volume->getChannel(0,scalar),"get scalar channel");
        if(snapshot.channelMask!=1||scalar.desc.format!=gvdb::portable::AtlasFormat::R32_FLOAT)
            throw std::runtime_error("render-to-file requires exactly one R32_FLOAT channel");
        gvdb::portable::LevelInfo leafLevel;check(volume->getLevel(0,leafLevel),"get leaf level");
        const auto& info=snapshot.volume;
        // Rendering requires only the retained snapshot; no duplicate atlas/topology owner.
        load=nullptr;serializer=nullptr;owner=nullptr;upload=nullptr;
        ethereal::slang::ComputeRuntime runtime(device.get(),shaderPath);
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
        const double maximumSteps=std::ceil(double(radius)*2/step)+8;
        if(maximumSteps>UINT32_MAX)throw std::invalid_argument("Ray step requires too many samples");
        p.image={uint32_t(width),uint32_t(height),256,uint32_t(maximumSteps)};
        p.rayOrigin=vector4(center+backwards*radius*2.8f);
        p.direction00=vector4(forward-right*horizontal+up*vertical);
        p.directionDx=vector4(right*(2*horizontal/width));p.directionDy=vector4(up*(-2*vertical/height));
        p.march={step,0,radius*6,1.5f};p.valueRange={0,1,0.1f,0.995f};
        p.background={0.1f,0.2f,0.4f,1};p.light=vector4(center+dm::float3(-radius,radius*2,-radius),0.1f);
        runtime.flush(); // Exclude asset upload from GPU render timing.
        const auto start=std::chrono::steady_clock::now();
        auto rendered=gvdb::sample::raycastOwnedVolume(runtime,volume,0,p,transferFunction(),surface);
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
            leafLevel.nodes.z,size_t(scalar.storage.voxelCount),changed,milliseconds);
        std::printf("Wrote %s\n",output.string().c_str());return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"render-to-file: %s\n",error.what());return 1;}
}

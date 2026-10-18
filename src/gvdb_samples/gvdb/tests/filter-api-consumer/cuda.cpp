// Installed public filter lifetime fixture; no CUDA SDK or concrete GVDB owner headers.
#include "GVDBNvrhiVoxelFilterApi.h"
#include <nvrhi/cuda.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <vector>
namespace g=gvdb::portable;
std::vector<char> read(const char* path) {
    std::ifstream file(path,std::ios::binary);
    return {(std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>()};
}
int main(int argc,char** argv) {
    using namespace nvrhi;
    if(argc!=3)return 1;
    const auto remap=read(argv[1]),artifact=read(argv[2]);
    cuda::DeviceDesc native;native.enableOptiX=false;
    DeviceHandle device=cuda::createDevice(native);if(!device)return 2;
    g::VolumeCreateDesc config;config.device=device;config.configuration.levelCount=1;config.configuration.logDimension={1};
    config.remapArtifact=remap.data();config.remapArtifactBytes=remap.size();
    g::VolumeHandle owner;if(NVRHI_FAILED(g::createVolume(config,&owner)))return 3;
    g::VoxelFiltersCreateDesc desc;desc.volume=owner;desc.artifact=artifact.data();desc.artifactBytes=artifact.size();
    g::VoxelFiltersHandle filters;if(NVRHI_FAILED(g::createVoxelFilters(desc,&filters)))return 4;
    auto* volume=filters->getVolume();owner=nullptr;
    g::NodeId leaf;
    if(NVRHI_FAILED(volume->activate(0,0,0,leaf))||NVRHI_FAILED(volume->finishTopology()))return 5;
    if(NVRHI_FAILED(volume->setChannel(0,{g::AtlasFormat::R32_FLOAT,{13,0,0,0}})))return 6;
    std::vector<float> data(64,8);
    const g::ChannelUploadDesc upload{0,0,data.data(),data.size()*sizeof(float)};
    g::GenerationDesc initial;initial.placement.minimumBrickCount={1,1,1};initial.uploads=&upload;initial.uploadCount=1;
    g::GenerationTicketHandle first;if(NVRHI_FAILED(volume->prepareGeneration(initial,&first)))return 7;
    CommandListHandle commands;if(NVRHI_FAILED(device->createCommandList({},&commands)))return 8;
    commands->open();const g::RecordingContext context{device,commands};
    if(NVRHI_FAILED(volume->recordAndPublish(context,first)))return 9;
    g::VoxelFilterRequest request;request.iterations=2;request.parameters={2,0,0,0};request.boundaryValue={2,0,0,0};request.skipOverAprons=0;
    g::GenerationTicketHandle ticket;if(NVRHI_FAILED(filters->prepare(request,{},&ticket)))return 10;
    if(NVRHI_FAILED(volume->recordAndPublish(context,ticket)))return 11;
    g::SnapshotHandle snapshot;if(NVRHI_FAILED(volume->getSnapshot(&snapshot)))return 12;
    g::ReadbackHandle readback;
    if(NVRHI_FAILED(volume->prepareReadback(snapshot,1,&readback))||NVRHI_FAILED(volume->recordReadback(context,readback)))return 13;
    first=nullptr;ticket=nullptr;snapshot=nullptr;filters=nullptr;
    commands->close();device->executeCommandList(commands);
    if(!device->waitForIdle()||NVRHI_FAILED(readback->finishAfterCallerCompletion()))return 14;
    g::BorrowedBytes bytes;if(NVRHI_FAILED(readback->getChannelData(0,bytes))||bytes.byteCount!=256)return 15;
    for(uint32_t z=0;z<4;++z)for(uint32_t y=0;y<4;++y)for(uint32_t x=0;x<4;++x) {
        float actual;std::memcpy(&actual,static_cast<const uint8_t*>(bytes.data)+4*((z*4+y)*4+x),4);
        const bool inside=x>=1&&x<=2&&y>=1&&y<=2&&z>=1&&z<=2;
        // First pass: (5*8+3*13)/8=9.875. Apron becomes2, then second=(5*9.875+6)/8.
        const float expected=inside?6.921875f:2.f;
        if(!std::isfinite(actual)||std::abs(actual-expected)>1e-6f)return 16;
    }
    readback=nullptr;commands=nullptr;device->runGarbageCollection();
    std::puts("Installed filters: retained owner, two-pass apron schedule and owner-free readback PASS");return 0;
}

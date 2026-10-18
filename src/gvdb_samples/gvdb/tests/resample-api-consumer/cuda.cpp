// Installed dense resample fixture; public API only, no CUDA SDK headers.
#include "GVDBNvrhiResampleApi.h"
#include <nvrhi/cuda.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
namespace g=gvdb::portable;
std::vector<char> read(const char* path) {
    std::ifstream file(path,std::ios::binary);
    return {(std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>()};
}
uint32_t bits(float value){uint32_t result;std::memcpy(&result,&value,4);return result;}
int main(int argc,char** argv) {
    using namespace nvrhi;
    if(argc!=3)return 1;
    const auto remap=read(argv[1]),artifact=read(argv[2]);
    cuda::DeviceDesc native;native.enableOptiX=false;
    DeviceHandle device=cuda::createDevice(native);if(!device)return 2;
    g::VolumeCreateDesc config;config.device=device;config.configuration.levelCount=1;config.configuration.logDimension={1};
    config.remapArtifact=remap.data();config.remapArtifactBytes=remap.size();
    g::VolumeHandle owner;if(NVRHI_FAILED(g::createVolume(config,&owner)))return 3;
    g::NodeId leaf;if(NVRHI_FAILED(owner->activate(0,0,0,leaf))||NVRHI_FAILED(owner->finishTopology()))return 4;
    if(NVRHI_FAILED(owner->setChannel(31,{g::AtlasFormat::R32_FLOAT,{9,0,0,0}})))return 5;
    std::vector<uint32_t> initial(128,bits(5));initial[(1*4+1)*8+2]=0x7fc12345u;initial[(1*4+2)*8+1]=0x80000000u;
    g::ChannelUploadDesc upload{31,0,initial.data(),initial.size()*4};
    g::GenerationDesc first;first.placement.minimumBrickCount={2,1,1};first.uploads=&upload;first.uploadCount=1;
    g::GenerationTicketHandle generation;if(NVRHI_FAILED(owner->prepareGeneration(first,&generation)))return 6;
    CommandListHandle commands;if(NVRHI_FAILED(device->createCommandList({},&commands)))return 7;
    commands->open();const g::RecordingContext context{device,commands};
    if(NVRHI_FAILED(owner->recordAndPublish(context,generation)))return 8;
    g::ResampleCreateDesc desc;desc.volume=owner;desc.artifact=artifact.data();desc.artifactBytes=artifact.size();
    g::ResampleHandle resample;if(NVRHI_FAILED(g::createResample(desc,&resample)))return 9;
    std::array<g::ReadbackHandle,2> results;
    for(uint32_t pass=0;pass<2;++pass) {
        float dense=7;
        g::ResampleRequestDesc request;request.channel=31;request.sourceData=&dense;request.sourceBytes=4;request.sourceResolution={1,1,1};
        request.remap={0,10,1,21};request.preserveOutside=pass==0;request.background={-3,0,0,0};
        if(NVRHI_FAILED(resample->prepare(request,{},&generation)))return 10;
        dense=123; // The prepared ticket must have copied7.
        if(NVRHI_FAILED(owner->recordAndPublish(context,generation)))return 11;
        g::SnapshotHandle snapshot;if(NVRHI_FAILED(owner->getSnapshot(&snapshot)))return 12;
        if(NVRHI_FAILED(owner->prepareReadback(snapshot,uint32_t(1)<<31,&results[pass]))||NVRHI_FAILED(owner->recordReadback(context,results[pass])))return 13;
    }
    owner=nullptr;resample=nullptr;generation=nullptr;
    commands->close();device->executeCommandList(commands);if(!device->waitForIdle())return 14;
    for(uint32_t pass=0;pass<2;++pass) {
        if(NVRHI_FAILED(results[pass]->finishAfterCallerCompletion()))return 15;
        g::BorrowedBytes bytes;if(NVRHI_FAILED(results[pass]->getChannelData(31,bytes))||bytes.byteCount!=512)return 16;
        for(uint32_t z=0;z<4;++z)for(uint32_t y=0;y<4;++y)for(uint32_t x=0;x<8;++x) {
            const uint32_t index=(z*4+y)*8+x;
            uint32_t expected=0,actual;std::memcpy(&actual,static_cast<const uint8_t*>(bytes.data)+index*4,4);
            if(x<4) {
                const bool interior=x>=1&&x<=2&&y>=1&&y<=2&&z>=1&&z<=2;
                expected=pass?bits(-3):interior?initial[index]:bits(9);
                if(x==1&&y==1&&z==1)expected=bits(15);
            }
            if(actual!=expected)return 17;
        }
    }
    results={};commands=nullptr;device->runGarbageCollection();
    std::puts("Installed resample: copied dense input, exact NaN/-0 preservation, background and old snapshots PASS");return 0;
}

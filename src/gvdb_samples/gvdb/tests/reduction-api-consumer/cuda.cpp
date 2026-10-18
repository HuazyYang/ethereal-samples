// Installed public reduction completion/lifetime fixture. No concrete GVDB or CUDA SDK headers.
#include "GVDBNvrhiReductionApi.h"
#include <nvrhi/cuda.h>
#include <cstdio>
#include <cstring>
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
    g::NodeId leaf;if(NVRHI_FAILED(owner->activate(0,0,0,leaf))||NVRHI_FAILED(owner->finishTopology()))return 4;
    if(NVRHI_FAILED(owner->setChannel(31,{g::AtlasFormat::R32_FLOAT,{1000,0,0,0}})))return 5;
    std::vector<float> data(128,9999);uint32_t value=1;
    for(uint32_t z=1;z<=2;++z)for(uint32_t y=1;y<=2;++y)for(uint32_t x=1;x<=2;++x)data[(z*4+y)*8+x]=float(value++);
    g::ChannelUploadDesc input{31,0,data.data(),data.size()*4};
    g::GenerationDesc initial;initial.placement.minimumBrickCount={2,1,1};initial.uploads=&input;initial.uploadCount=1;
    g::GenerationTicketHandle generation;if(NVRHI_FAILED(owner->prepareGeneration(initial,&generation)))return 6;
    CommandListHandle commands;if(NVRHI_FAILED(device->createCommandList({},&commands)))return 7;
    commands->open();const g::RecordingContext context{device,commands};
    if(NVRHI_FAILED(owner->recordAndPublish(context,generation)))return 8;
    g::SnapshotHandle snapshot;if(NVRHI_FAILED(owner->getSnapshot(&snapshot)))return 9;
    g::ReductionCreateDesc desc;desc.volume=owner;desc.artifact=artifact.data();desc.artifactBytes=artifact.size();
    g::ReductionHandle reduction;if(NVRHI_FAILED(g::createReduction(desc,&reduction)))return 10;
    g::ReductionTicketHandle ticket;if(NVRHI_FAILED(reduction->prepare(snapshot,31,{},&ticket)))return 11;
    g::ReductionValue result{123,456,789};
    if(NVRHI_SUCCEEDED(ticket->finishAfterCallerCompletion(result))||result.sum!=123||result.reserved!=456||result.voxelCount!=789)return 12;
    if(NVRHI_FAILED(reduction->record(context,ticket)))return 13;
    if(NVRHI_SUCCEEDED(reduction->record(context,ticket)))return 14;
    owner=nullptr;reduction=nullptr;snapshot=nullptr;generation=nullptr;
    commands->close();device->executeCommandList(commands);
    if(!device->waitForIdle()||NVRHI_FAILED(ticket->finishAfterCallerCompletion(result)))return 15;
    if(result.sum!=36||result.voxelCount!=8||result.reserved)return 16;
    result={123,456,789};
    if(NVRHI_SUCCEEDED(ticket->finishAfterCallerCompletion(result))||result.sum!=123||result.reserved!=456||result.voxelCount!=789)return 17;
    ticket=nullptr;commands=nullptr;device->runGarbageCollection();
    std::puts("Installed reduction: channel31 interiors only, one-use state and owner-free completion PASS");return 0;
}

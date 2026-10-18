// Installed public point phases; no CUDA SDK or concrete GVDB owner headers.
#include "GVDBNvrhiPointApi.h"
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
    g::VolumeCreateDesc config;config.device=device;config.configuration.levelCount=4;config.configuration.logDimension={2,1,1,1};
    config.remapArtifact=remap.data();config.remapArtifactBytes=remap.size();
    g::VolumeHandle owner;if(NVRHI_FAILED(g::createVolume(config,&owner)))return 3;
    if(NVRHI_FAILED(owner->setChannel(0,{g::AtlasFormat::R32_FLOAT,{5,0,0,0}})))return 4;
    const auto epoch=owner->getTopologyRevision().epoch;
    g::PointOpsCreateDesc desc;desc.volume=owner;desc.conversionArtifact=artifact.data();desc.conversionArtifactBytes=artifact.size();
    g::PointOpsHandle points;if(NVRHI_FAILED(g::createPointOps(desc,&points)))return 5;
    std::vector<double> source={-.625,.125,.125,1.625,.125,.125};
    g::PointSetDesc input;input.positions.stream={source.data(),source.size()*8,0,24,2,g::PointComponent::Float64,3};
    input.positions.transform.scale={2,2,2,2};input.positions.transform.translation={1,1,1,1};
    g::PointUploadTicketHandle upload;if(NVRHI_FAILED(points->preparePoints(input,{},&upload)))return 6;
    source.assign(source.size(),99);source.clear();source.shrink_to_fit();
    CommandListHandle commands;if(NVRHI_FAILED(device->createCommandList({},&commands)))return 7;
    commands->open();const g::RecordingContext context{device,commands};
    if(NVRHI_FAILED(points->recordPoints(context,upload)))return 8;
    points=nullptr;
    commands->close();device->executeCommandList(commands);if(!device->waitForIdle())return 9;
    g::PointSetHandle dataset;if(NVRHI_FAILED(upload->finishAfterCallerCompletion(&dataset)))return 10;
    g::BorrowedBytes converted;if(NVRHI_FAILED(dataset->getData(g::PointAttribute::Position,converted))||converted.byteCount!=24)return 11;
    const float expected[]={-.25f,1.25f,1.25f,4.25f,1.25f,1.25f};
    if(std::memcmp(converted.data,expected,sizeof(expected)))return 12;
    auto* repeated=reinterpret_cast<g::IGVDBPointSet*>(uintptr_t(1));
    if(NVRHI_SUCCEEDED(upload->finishAfterCallerCompletion(&repeated))||repeated)return 13;
    if(NVRHI_FAILED(g::createPointOps(desc,&points)))return 14; // Same-device PointSet reusable by another factory.
    g::PointTopologyDesc topology;topology.mode=g::PointTopologyMode::Accumulate;topology.minDepth=2;
    g::PlacementDesc placement;placement.minimumBrickCount={1,1,1};
    g::GenerationTicketHandle generation;
    if(NVRHI_FAILED(points->prepareTopology(dataset,topology,placement,{},&generation)))return 15;
    commands->open();if(NVRHI_FAILED(owner->recordAndPublish(context,generation)))return 16;
    // Signed fractional center is unioned with truncated symmetric radius-zero support.
    if(!owner->isActive({-4,0,0})||!owner->isActive({0,0,0})||!owner->isActive({4,0,0})
        ||owner->getRootId().level()<2||owner->getTopologyRevision().epoch!=epoch)return 17;
    g::SnapshotHandle snapshot;if(NVRHI_FAILED(owner->getSnapshot(&snapshot)))return 18;
    g::ReadbackHandle readback;
    if(NVRHI_FAILED(owner->prepareReadback(snapshot,1,&readback))||NVRHI_FAILED(owner->recordReadback(context,readback)))return 19;
    owner=nullptr;points=nullptr;dataset=nullptr;upload=nullptr;snapshot=nullptr;generation=nullptr;
    commands->close();device->executeCommandList(commands);
    if(!device->waitForIdle()||NVRHI_FAILED(readback->finishAfterCallerCompletion()))return 20;
    g::BorrowedBytes bytes;if(NVRHI_FAILED(readback->getChannelData(0,bytes)))return 21;
    size_t nonzero=0;
    for(size_t i=0;i<bytes.byteCount;i+=4) {
        float value;std::memcpy(&value,static_cast<const uint8_t*>(bytes.data)+i,4);
        if(value!=0&&value!=5)return 22;
        nonzero+=value==5;
    }
    if(nonzero!=3*216)return 23; // Three six-cubed bricks including aprons; unused slots are zero.
    readback=nullptr;commands=nullptr;device->runGarbageCollection();
    std::puts("Installed points: copied input, explicit GPU completion, signed append and owner-free readback PASS");return 0;
}

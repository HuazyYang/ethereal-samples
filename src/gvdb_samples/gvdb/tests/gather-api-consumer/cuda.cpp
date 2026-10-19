// Installed gather fixture: public API and NVRHI CUDA factory, no native SDK headers.
#include "GVDBNvrhiGatherApi.h"
#include <nvrhi/cuda.h>
#include <array>
#include <algorithm>
#include <cmath>
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
#define REQUIRE(c) do {if(!(c)){std::fprintf(stderr,"Installed gather failed at line %d\n",__LINE__);return 1;}}while(false)
#define CHECK(c) REQUIRE(NVRHI_SUCCEEDED(c))
int main(int argc,char** argv) {
    using namespace nvrhi;
    REQUIRE(argc==4||argc==5);
    const uint32_t passes=argc==5?4:3;
    const auto remap=read(argv[1]),conversion=read(argv[2]),artifact=read(argv[3]);
    cuda::DeviceDesc native;native.enableOptiX=false;
    DeviceHandle device=cuda::createDevice(native);REQUIRE(device);
    g::VolumeCreateDesc config;config.device=device;config.configuration.levelCount=1;config.configuration.logDimension={2};
    config.remapArtifact=remap.data();config.remapArtifactBytes=remap.size();
    g::VolumeHandle owner;CHECK(g::createVolume(config,&owner));
    g::NodeId leaf;CHECK(owner->activate(0,0,0,leaf));CHECK(owner->finishTopology());
    CHECK(owner->setChannel(31,{g::AtlasFormat::R32_FLOAT,{9,0,0,0}}));
    CHECK(owner->setChannel(7,{g::AtlasFormat::RGBA8_UINT,{5,6,7,8}}));
    g::GenerationDesc initial;initial.placement.minimumBrickCount={2,1,1};
    g::GenerationTicketHandle generation;CHECK(owner->prepareGeneration(initial,&generation));
    CommandListHandle commands;CHECK(device->createCommandList({},&commands));
    commands->open();const g::RecordingContext context{device,commands};
    CHECK(owner->recordAndPublish(context,generation));
    g::PointOpsCreateDesc pointDesc;pointDesc.volume=owner;pointDesc.conversionArtifact=conversion.data();pointDesc.conversionArtifactBytes=conversion.size();
    g::PointOpsHandle pointFactory;CHECK(g::createPointOps(pointDesc,&pointFactory));
    g::Float4 position{0,0,0,99};std::array<uint8_t,4> color{32,64,128,255};
    g::PointSetDesc input;input.positions.stream={&position,sizeof(position),0,sizeof(position),1,g::PointComponent::Float32,4};
    input.colors.stream={color.data(),color.size(),0,4,1,g::PointComponent::UInt8,4};
    g::PointUploadTicketHandle pointTicket;CHECK(pointFactory->preparePoints(input,{},&pointTicket));
    position={99,99,99,99};color.fill(0); // Conversion copied the original associated inputs.
    CHECK(pointFactory->recordPoints(context,pointTicket));
    pointFactory=nullptr;
    commands->close();device->executeCommandList(commands);REQUIRE(device->waitForIdle());
    g::PointSetHandle points;CHECK(pointTicket->finishAfterCallerCompletion(&points));pointTicket=nullptr;
    g::GatherCreateDesc create;create.volume=owner;create.artifact=artifact.data();create.artifactBytes=artifact.size();
    g::GatherHandle gather;CHECK(g::createGather(create,&gather));
    REQUIRE(gather->getVolume()==owner.Get());
    g::GatherEvaluationHandle extension;
    CHECK(gather->QueryInterface(uuid_of<g::IGVDBGatherEvaluation>(),reinterpret_cast<void**>(extension.GetAddressOf())));
    g::GatherRequestDesc request;request.points=points;request.channel=31;request.colorChannel=7;request.subcellWidth=1;request.radius=1;request.supportRadius=2;
    g::GatherHandle weighted;
    if(argc==5) {
        const auto binary=read(argv[4]);
        create.artifact=binary.data();create.artifactBytes=binary.size();
        CHECK(g::createInverseDistanceGather(create,&weighted));
    }
    std::array<g::ReadbackHandle,4> results;
    commands->open();
    for(uint32_t pass=0;pass<passes;++pass) {
        request.kind=pass==0?g::GatherKind::Density:g::GatherKind::LevelSet;
        request.translation={pass==0||pass==3?0.f:100.f,0,0,0};request.accumulate=pass==2;
        if(pass==0) {
            g::GatherEvaluationDesc evaluation;evaluation.request=request;evaluation.evaluationPoints=points;
            CHECK(extension->prepareEvaluation(evaluation,{},&generation));
        } else if(pass==3) CHECK(weighted->prepare(request,{},&generation));
        else CHECK(gather->prepare(request,{},&generation));
        if(pass+1==passes)points=nullptr; // Prepared commands must retain GPU streams themselves.
        CHECK(owner->recordAndPublish(context,generation));
        g::SnapshotHandle snapshot;CHECK(owner->getSnapshot(&snapshot));
        CHECK(owner->prepareReadback(snapshot,(uint32_t(1)<<31)|(uint32_t(1)<<7),&results[pass]));
        CHECK(owner->recordReadback(context,results[pass]));
    }
    owner=nullptr;gather=nullptr;weighted=nullptr;extension=nullptr;generation=nullptr;
    commands->close();device->executeCommandList(commands);REQUIRE(device->waitForIdle());
    for(uint32_t pass=0;pass<passes;++pass) {
        CHECK(results[pass]->finishAfterCallerCompletion());
        g::BorrowedBytes scalar,colors;CHECK(results[pass]->getChannelData(31,scalar));CHECK(results[pass]->getChannelData(7,colors));
        REQUIRE(scalar.byteCount==1728&&colors.byteCount==1728);
        for(uint32_t z=0;z<6;++z)for(uint32_t y=0;y<6;++y)for(uint32_t x=0;x<12;++x) {
            const uint32_t index=(z*6+y)*12+x;
            uint32_t actualScalar,actualColor;std::memcpy(&actualScalar,static_cast<const uint8_t*>(scalar.data)+index*4,4);
            std::memcpy(&actualColor,static_cast<const uint8_t*>(colors.data)+index*4,4);
            uint32_t expectedScalar=0,expectedColor=0;
            if(x<6) {
                const bool interior=x>=1&&x<=4&&y>=1&&y<=4&&z>=1&&z<=4;
                expectedScalar=interior?(pass?bits(3):bits(x==1&&y==1&&z==1?1.f:0.f)):bits(9);
                expectedColor=interior?(pass?0u:0xfe804020u):0x08070605u;
            }
            if(pass==3&&x>=1&&x<=4&&y>=1&&y<=4&&z>=1&&z<=4) {
                const double distance=std::sqrt(double((x-1)*(x-1)+(y-1)*(y-1)+(z-1)*(z-1)));
                float actual;std::memcpy(&actual,&actualScalar,4);
                REQUIRE(std::abs(actual-std::min(3.,distance-1))<2e-6);
                const unsigned bytes[]={32,64,128,255};
                for(uint32_t lane=0;lane<4;++lane) {
                    const double ideal=distance?std::min(255.,bytes[lane]/distance):255.;
                    const uint32_t lo=uint32_t(std::max(0.,ideal-1e-4)),hi=uint32_t(std::min(255.,ideal+1e-4));
                    const uint32_t actualByte=(actualColor>>(lane*8))&255;
                    REQUIRE(actualByte>=lo&&actualByte<=hi);
                }
            } else REQUIRE(actualScalar==expectedScalar&&actualColor==expectedColor);
        }
    }
    results={};commands=nullptr;device->runGarbageCollection();
    std::puts("Installed gather: retained streams, independent support/color, alpha wrap, reset/accumulate, aprons and snapshots PASS");return 0;
}

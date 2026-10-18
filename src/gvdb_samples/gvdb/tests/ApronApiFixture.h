// SPDX-License-Identifier: Apache-2.0
// Independent world-coordinate oracle shared by native and installed public consumers.
#pragma once
#include "GVDBNvrhiApronApi.h"
#include "GVDBNvrhiVoxelApi.h"
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
namespace apron_fixture {
namespace g=gvdb::portable;using namespace nvrhi;
inline void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
inline void check(FRESULT result,const char* text){if(NVRHI_FAILED(result))throw std::runtime_error(std::string(text)+": "+std::to_string(result));}
constexpr uint32_t channels[]={0,1,2,3,31};
constexpr uint32_t strides[]={1,4,4,12,16};
constexpr g::AtlasFormat formats[]={g::AtlasFormat::R8_UINT,g::AtlasFormat::RGBA8_UINT,g::AtlasFormat::R32_FLOAT,g::AtlasFormat::RGB32_FLOAT,g::AtlasFormat::RGBA32_FLOAT};
constexpr uint32_t selected=0x8000000fu,untouched=1u<<7;
inline g::Float4 defaultValue(){return {7,8,9,10};}
inline g::Float4 overrideValue(uint32_t format){
    switch(format){case 0:return {31,0,0,0};case 1:return {101,102,103,104};case 2:return {-0.f,2,3,4};case 3:return {-.25f,1.25f,-2.5f,3};default:return {9,-4,.125f,-0.f};}
}
using Bytes=std::array<uint8_t,16>;
inline Bytes encode(uint32_t format,g::Float4 value){
    Bytes result{};
    if(format==0)result[0]=uint8_t(value.x);
    else if(format==1){result[0]=uint8_t(value.x);result[1]=uint8_t(value.y);result[2]=uint8_t(value.z);result[3]=uint8_t(value.w);}
    else std::memcpy(result.data(),&value,strides[format]);
    return result;
}
inline Bytes interior(uint32_t format,int x,int y,int z){
    Bytes result{};
    if(format==0)result[0]=uint8_t(100+x+2+10*y+20*z);
    else if(format==1){result[0]=uint8_t(22+x);result[1]=uint8_t(40+y);result[2]=uint8_t(60+z);result[3]=uint8_t(80+x+2+y+z);}
    else for(uint32_t component=0;component<strides[format]/4;++component){
        const float value=100.f*format+x+2+10*y+20*z+component*.25f;
        uint32_t bits;std::memcpy(&bits,&value,4);
        if(component==0&&x==-2&&y==0&&z==0)bits=0x7fc12345u+format;
        if(component==0&&x==0&&y==1&&z==1)bits=0x80000000u;
        std::memcpy(result.data()+component*4,&bits,4);
    }
    return result;
}
inline g::VolumeHandle makeVolume(IDevice* device,const std::vector<char>& artifact){
    g::VolumeCreateDesc desc;desc.device=device;desc.configuration.levelCount=2;desc.configuration.logDimension={1,1};
    desc.remapArtifact=artifact.data();desc.remapArtifactBytes=artifact.size();
    g::VolumeHandle output;check(g::createVolume(desc,&output),"create volume");return output;
}
inline g::SnapshotHandle snapshot(g::IGVDBVolume* owner){g::SnapshotHandle result;check(owner->getSnapshot(&result),"snapshot");return result;}
inline g::ChannelSnapshotDesc channel(g::IGVDBSnapshot* snapshot,uint32_t index){g::ChannelSnapshotDesc result;check(snapshot->getChannel(index,result),"channel");return result;}
inline void unchanged(g::IGVDBSnapshot* before,g::IGVDBSnapshot* after,uint32_t mask){
    const auto a=before->getDesc(),b=after->getDesc();
    require(a.revision.epoch==b.revision.epoch&&a.revision.topology==b.revision.topology&&a.revision.placement==b.revision.placement,"topology/placement identity");
    for(auto type:{g::TopologyBuffer::Nodes,g::TopologyBuffer::Children,g::TopologyBuffer::Levels,g::TopologyBuffer::AtlasMap}){
        g::BorrowedBufferBinding old,now;check(before->getTopologyBuffer(type,old),"old topology binding");check(after->getTopologyBuffer(type,now),"new topology binding");
        require(old.buffer==now.buffer&&old.byteOffset==now.byteOffset&&old.byteSize==now.byteSize,"unchanged topology buffers");
    }
    for(uint32_t index=0;index<32;++index)if(mask&(1u<<index)){
        const auto old=channel(before,index),now=channel(after,index);
        require(old.raw.buffer==now.raw.buffer&&old.raw.byteSize==now.raw.byteSize&&old.revision.resource==now.revision.resource&&old.revision.content==now.revision.content,"unchanged binding/revision");
    }
}
inline void metadata(g::IGVDBSnapshot* value){for(uint32_t f=0;f<5;++f){
    const auto desc=channel(value,channels[f]).desc;const auto expected=defaultValue();
    require(desc.format==formats[f]&&!std::memcmp(&desc.voidValue,&expected,sizeof(expected)),"override never mutates descriptor");
}}
inline void verify(g::IGVDBReadback* readback,uint32_t stage){
    for(uint32_t f=0;f<5;++f){
        g::BorrowedBytes bytes;check(readback->getChannelData(channels[f],bytes),"read format bytes");
        require(bytes.byteCount==192u*strides[f],"logical storage length");
        for(uint32_t z=0;z<4;++z)for(uint32_t y=0;y<4;++y)for(uint32_t x=0;x<12;++x){
            Bytes expected{};
            if(x<8){
                const int worldX=int(x/4)*2-2+int(x%4)-1,worldY=int(y)-1,worldZ=int(z)-1;
                if(worldX>=-2&&worldX<2&&worldY>=0&&worldY<2&&worldZ>=0&&worldZ<2)expected=interior(f,worldX,worldY,worldZ);
                else expected=encode(f,(stage==1||((stage==3||stage==5)&&f==1))?overrideValue(f):defaultValue());
            }
            const size_t offset=((size_t(z)*4+y)*12+x)*strides[f];
            if(std::memcmp(static_cast<const uint8_t*>(bytes.data)+offset,expected.data(),strides[f]))
                throw std::runtime_error("apron oracle stage="+std::to_string(stage)+" format="+std::to_string(f)+" xyz="+std::to_string(x)+","+std::to_string(y)+","+std::to_string(z));
        }
    }
}
inline void run(IDevice* device,const std::vector<char>& remap,const std::vector<char>& artifact){
    auto owner=makeVolume(device,remap);
    g::NodeId left,right;check(owner->activate(-2,0,0,left),"signed left leaf");check(owner->activate(0,0,0,right),"adjacent right leaf");check(owner->finishTopology(),"finish topology");
    for(uint32_t f=0;f<5;++f)check(owner->setChannel(channels[f],{formats[f],defaultValue()}),"set five formats");
    check(owner->setChannel(7,{g::AtlasFormat::R32_FLOAT,{42,0,0,0}}),"untouched channel");
    std::array<std::vector<uint8_t>,5> data;std::array<g::ChannelUploadDesc,5> uploads;
    for(uint32_t f=0;f<5;++f){
        data[f].assign(192*strides[f],0xcc);
        for(uint32_t slot=0;slot<2;++slot)for(uint32_t z=0;z<2;++z)for(uint32_t y=0;y<2;++y)for(uint32_t x=0;x<2;++x){
            const auto value=interior(f,int(slot)*2-2+int(x),int(y),int(z));
            const size_t index=((size_t(z+1)*4+y+1)*12+slot*4+x+1)*strides[f];
            std::memcpy(data[f].data()+index,value.data(),strides[f]);
        }
        uploads[f]={channels[f],0,data[f].data(),data[f].size()};
    }
    g::GenerationDesc initial;initial.placement.minimumBrickCount={3,1,1};initial.uploads=uploads.data();initial.uploadCount=5;
    g::GenerationTicketHandle ticket;check(owner->prepareGeneration(initial,&ticket),"prepare signed upload");
    CommandListHandle commands;check(device->createCommandList({},&commands),"commands");commands->open();g::RecordingContext context{device,commands};
    check(owner->recordAndPublish(context,ticket),"publish upload");
    g::ApronCreateDesc create;create.volume=owner;create.artifact=artifact.data();create.artifactBytes=artifact.size();
    g::ApronOpsHandle apron;check(g::createApronOps(create,&apron),"create apron");require(apron->getVolume()==owner.Get(),"retained owner");
    AutoPtr<g::IGVDBApronOps> same;check(apron->QueryInterface(uuid_of<g::IGVDBApronOps>(),reinterpret_cast<void**>(same.GetAddressOf())),"self QI");require(same==apron,"self identity");same=nullptr;
    void* missing=reinterpret_cast<void*>(uintptr_t(1));require(apron->QueryInterface(uuid_of<g::IGVDBVoxelOps>(),&missing)==FE_NOINTERFACE&&!missing,"new interface keeps old IID separate");
    std::array<g::ApronBoundaryOverride,5> values;for(uint32_t f=0;f<5;++f){values[f].channel=channels[f];values[f].value=overrideValue(f);}
    g::ApronRequestDesc request;request.channelMask=selected;request.overrides=values.data();request.overrideCount=5;
    std::array<g::SnapshotHandle,6> stages;stages[0]=snapshot(owner);metadata(stages[0]);
    auto reject=[&](g::ApronRequestDesc bad,g::ApronPreparationBudget budget={}){
        auto* output=reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));
        require(NVRHI_FAILED(apron->prepare(bad,budget,&output))&&!output,"rejected request clears output");unchanged(stages[0],snapshot(owner),selected|untouched);
    };
    {auto bad=request;--bad.structSize;reject(bad);bad=request;bad.reserved=1;reject(bad);bad=request;bad.reserved2=1;reject(bad);bad=request;bad.overrides=nullptr;reject(bad);bad=request;bad.overrideCount=33;reject(bad);bad=request;bad.channelMask=0;reject(bad);}
    for(uint32_t reserved=0;reserved<3;++reserved){values[0].reserved[reserved]=1;reject(request);values[0].reserved[reserved]=0;}
    values[1].channel=0;reject(request);values[1].channel=1;
    values[0].channel=32;reject(request);values[0].channel=0;
    values[0].value.x=.5f;reject(request);values[0].value=overrideValue(0);
    values[1].value.w=256;reject(request);values[1].value=overrideValue(1);
    values[0].value.w=std::numeric_limits<float>::infinity();reject(request);values[0].value=overrideValue(0);
    reject(request,{0,UINT32_MAX,0});reject(request,{UINT64_MAX,1,0});reject(request,{UINT64_MAX,UINT32_MAX,1});
    g::GenerationTicketHandle stale;check(apron->prepare(request,{},&stale),"prepare later stale ticket");
    check(apron->prepare(request,{},&ticket),"prepare five boundary overrides");
    for(auto& value:values)value.value={}; // Preparation copies values into retained command constants.
    auto foreign=makeVolume(device,remap);require(NVRHI_FAILED(foreign->recordAndPublish(context,ticket)),"foreign owner rejects without consuming");foreign=nullptr;
    check(owner->recordAndPublish(context,ticket),"publish overrides");require(NVRHI_FAILED(owner->recordAndPublish(context,stale)),"stale ticket rejected");
    stages[1]=snapshot(owner);metadata(stages[1]);unchanged(stages[0],stages[1],untouched);
    for(uint32_t f=0;f<5;++f)require(channel(stages[0],channels[f]).raw.buffer!=channel(stages[1],channels[f]).raw.buffer,"override fresh resource");
    request.overrides=nullptr;request.overrideCount=0;check(apron->prepare(request,{},&ticket),"prepare descriptor defaults");check(owner->recordAndPublish(context,ticket),"restore defaults");stages[2]=snapshot(owner);metadata(stages[2]);
    values[0]={};values[0].channel=1;values[0].value=overrideValue(1);request.channelMask=selected;request.overrides=values.data();request.overrideCount=1;
    check(apron->prepare(request,{},&ticket),"prepare mixed selected defaults and one override");check(owner->recordAndPublish(context,ticket),"publish subset");stages[3]=snapshot(owner);metadata(stages[3]);unchanged(stages[2],stages[3],untouched);
    g::VoxelOpsCreateDesc oldCreate;oldCreate.volume=owner;oldCreate.artifact=artifact.data();oldCreate.artifactBytes=artifact.size();
    g::VoxelOpsHandle old;check(g::createVoxelOps(oldCreate,&old),"old voxel factory remains supported");check(old->prepareApron(selected,{},&ticket),"old apron overload");check(owner->recordAndPublish(context,ticket),"old defaults publication");stages[4]=snapshot(owner);metadata(stages[4]);
    request={};check(apron->prepare(request,{0,0,0},&ticket),"zero mask no-op");check(owner->recordAndPublish(context,ticket),"consume no-op");require(NVRHI_FAILED(owner->recordAndPublish(context,ticket)),"no-op one-use");unchanged(stages[4],snapshot(owner),selected|untouched);
    request.channelMask=1u<<1;request.overrides=values.data();request.overrideCount=1;
    check(apron->prepare(request,{},&ticket),"prepare selected subset");check(owner->recordAndPublish(context,ticket),"publish selected subset");stages[5]=snapshot(owner);metadata(stages[5]);unchanged(stages[4],stages[5],(selected|untouched)&~(1u<<1));
    // Validation precedes the empty/apron0 shortcuts, and valid no-ops allocate nothing.
    for(bool empty:{false,true}){
        auto minimal=makeVolume(device,remap);if(!empty){g::NodeId node;check(minimal->activate(0,0,0,node),"minimal leaf");}check(minimal->finishTopology(),"finish minimal");
        check(minimal->setChannel(0,{g::AtlasFormat::R8_UINT,{7,0,0,0}}),"minimal channel");
        g::GenerationDesc prepare;prepare.placement.minimumBrickCount={1,1,1};prepare.placement.apron=empty?1:0;
        g::GenerationTicketHandle initialTicket;check(minimal->prepareGeneration(prepare,&initialTicket),"minimal generation");check(minimal->recordAndPublish(context,initialTicket),"minimal publish");
        auto before=snapshot(minimal);create.volume=minimal;g::ApronOpsHandle op;check(g::createApronOps(create,&op),"minimal factory");
        g::ApronBoundaryOverride boundary;boundary.value={.5f,0,0,0};g::ApronRequestDesc r;r.channelMask=1;r.overrideCount=1;r.overrides=&boundary;
        auto* output=reinterpret_cast<g::IGVDBGenerationTicket*>(uintptr_t(1));require(op->prepare(r,{0,0,0},&output)==FE_INVALID_ARGS&&!output,"no-op still validates integer boundary");
        boundary.value={13,0,0,0};check(op->prepare(r,{0,0,0},&initialTicket),"valid allocation-free no-op");check(minimal->recordAndPublish(context,initialTicket),"minimal no-op publication");unchanged(before,snapshot(minimal),1);
        require(NVRHI_FAILED(minimal->recordAndPublish(context,initialTicket)),"minimal no-op one-use");
    }
    std::array<g::ReadbackHandle,6> results;
    for(uint32_t stage=0;stage<6;++stage){check(owner->prepareReadback(stages[stage],selected,&results[stage]),"prepare retained readback");check(owner->recordReadback(context,results[stage]),"record retained readback");}
    owner=nullptr;apron=nullptr;old=nullptr;ticket=nullptr;stale=nullptr;stages={};
    commands->close();device->executeCommandList(commands);require(device->waitForIdle(),"explicit caller completion");
    for(uint32_t stage=0;stage<6;++stage){check(results[stage]->finishAfterCallerCompletion(),"owner-free completion");verify(results[stage],stage);}
}
} // namespace apron_fixture

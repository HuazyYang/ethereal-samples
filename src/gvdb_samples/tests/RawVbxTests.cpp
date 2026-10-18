// Exact all-format and multi-channel VBX preservation tests.
// SPDX-License-Identifier: Apache-2.0
#include "PortableFixtures.h"
#include "../gvdb/slang/GVDBPortableVbx.h"
#include <iostream>
#include <stdexcept>
using namespace gvdb::slang;
static void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
static void word(std::vector<uint8_t>& b,size_t p,uint32_t v){for(unsigned i=0;i<4;++i)b.at(p+i)=uint8_t(v>>(i*8));}
int main(){try{
    auto fixture=fixtures::twoBrickVolume();VbxRawVolume input;std::string error;
    require(packVolume(fixtures::volumeSource(fixture),input.packed,error),error.c_str());
    input.transform.translation={1,2,3};input.voxelSize={.5f,1,2};input.gridName[0]='m';
    for(uint32_t format=0;format<5;++format){VbxChannel channel;channel.format=AtlasFormat(format);channel.bytes.resize(128*atlasStride(channel.format));
        for(size_t i=0;i<channel.bytes.size();++i)channel.bytes[i]=uint8_t(i*37+format*53);
        input.channels.push_back(std::move(channel));
    }
    std::vector<uint8_t> bytes;require(saveRawVBX(input,bytes,error),error.c_str());VbxRawVolume loaded;
    require(loadRawVBX(bytes.data(),bytes.size(),loaded,error),error.c_str());
    require(loaded.channels.size()==5,"multi-channel count");
    require(loaded.packed.nodes==input.packed.nodes&&loaded.packed.children==input.packed.children,"multi-channel topology");
    require(std::memcmp(&loaded.transform,&input.transform,sizeof(VbxTransform))==0&&loaded.gridName==input.gridName,"multi-channel metadata");
    for(size_t i=0;i<5;++i)require(loaded.channels[i].format==input.channels[i].format&&loaded.channels[i].bytes==input.channels[i].bytes,"raw channel byte or format preservation");
    for(size_t i=0;i<5;++i){auto single=input;single.channels={input.channels[i]};std::vector<uint8_t> one;require(saveRawVBX(single,one,error),error.c_str());VbxRawVolume back;require(loadRawVBX(one.data(),one.size(),back,error),error.c_str());require(back.channels[0].bytes==single.channels[0].bytes,"single channel roundtrip");}
    VbxVolume scalar;require(!loadVBX(bytes.data(),bytes.size(),scalar,error),"scalar API accepted multiple channels");
    const auto original=loaded.channels[0].bytes;
    const auto reject=[&](const std::vector<uint8_t>& bad){require(!loadRawVBX(bad.data(),bad.size(),loaded,error),"corrupt multi-channel VBX accepted");require(!error.empty()&&loaded.channels[0].bytes==original,"failure changed output");};
    auto bad=bytes;bad.pop_back();reject(bad);bad=bytes;word(bad,735,6);reject(bad);bad=bytes;word(bad,739,4);reject(bad);
    bad=bytes;word(bad,357,1);reject(bad);bad=bytes;word(bad,353,0);reject(bad);bad=bytes;word(bad,353,UINT32_MAX);reject(bad);
    auto invalid=input;invalid.channels[3].bytes.pop_back();std::vector<uint8_t> destination={1,2,3};require(!saveRawVBX(invalid,destination,error)&&destination==std::vector<uint8_t>({1,2,3}),"invalid save changed output");
    std::cout<<"All five VBX channel formats and multi-channel corrupt-input tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

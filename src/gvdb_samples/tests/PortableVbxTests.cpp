// SPDX-License-Identifier: Apache-2.0
#include "PortableFixtures.h"
#include "../gvdb/slang/GVDBPortableVbx.h"
#include <iostream>
#include <stdexcept>
using namespace gvdb::slang;
static void require(bool c,const char* s){if(!c)throw std::runtime_error(s);}
static void write32(std::vector<uint8_t>& b,size_t p,uint32_t v){for(unsigned i=0;i<4;++i)b[p+i]=uint8_t(v>>(8*i));}
static void write64(std::vector<uint8_t>& b,size_t p,uint64_t v){write32(b,p,uint32_t(v));write32(b,p+4,uint32_t(v>>32));}
static void equal(const VbxVolume& a,const VbxVolume& b){
    require(a.packed.nodes==b.packed.nodes&&a.packed.children==b.packed.children,"topology roundtrip");
    require(a.atlas.size()==b.atlas.size()&&std::memcmp(a.atlas.data(),b.atlas.data(),a.atlas.size()*4)==0,"scalar atlas byte roundtrip");
    require(std::memcmp(&a.transform,&b.transform,sizeof(VbxTransform))==0,"transform roundtrip");
    require(a.gridName==b.gridName,"grid name roundtrip");
    require(std::memcmp(&a.voxelSize,&b.voxelSize,sizeof(VbxFloat3))==0,"voxel size roundtrip");
}
int main(int argc,char** argv){try{
    auto fixture=fixtures::twoBrickVolume();VbxVolume input;std::string error;
    require(packVolume(fixtures::volumeSource(fixture),input.packed,error),"fixture packing");
    input.atlas=fixture.atlas;input.transform.preTranslation={-2,3,4};input.transform.angles={10,20,30};
    input.transform.scale={2,3,4};input.transform.translation={5,-6,7};input.voxelSize={0.5f,1,2};input.gridName[0]='v';
    std::vector<uint8_t> bytes;require(saveVBX(input,bytes,error),"fixture save");
    VbxVolume loaded;if(!loadVBX(bytes.data(),bytes.size(),loaded,error))throw std::runtime_error(error);equal(input,loaded);
    const auto original=loaded.packed.nodes;
    const auto reject=[&](const std::vector<uint8_t>& bad,const char* name){require(!loadVBX(bad.data(),bad.size(),loaded,error),name);require(!error.empty()&&loaded.packed.nodes==original,"failure changed output");};
    for(size_t length:{size_t(0),size_t(1),size_t(6),size_t(61),size_t(320),bytes.size()-1})reject(std::vector<uint8_t>(bytes.begin(),bytes.begin()+length),"truncated VBX accepted");
    auto bad=bytes;bad[320]=1;reject(bad,"compression accepted");bad=bytes;bad[318]='i';reject(bad,"dtype accepted");
    bad=bytes;write32(bad,353,2);reject(bad,"multiple channels accepted");bad=bytes;write64(bad,54,bytes.size()+1);reject(bad,"invalid grid offset accepted");
    bad=bytes;write32(bad,395,11);reject(bad,"excessive levels accepted");bad=bytes;write32(bad,407,31);reject(bad,"overflowing dimension accepted");
    fixture.nodes[18]=2;fixture.atlasMap[1].y=2;fixture.volume.boundsMax.y=4;
    fixture.children[6]=fixture.children[2];fixture.children[7]=fixture.children[3];fixture.children[2]=fixture.children[3]=UINT32_MAX;
    VbxVolume maskInput;require(packVolume(fixtures::volumeSource(fixture),maskInput.packed,error),"mask fixture packing");maskInput.atlas=fixture.atlas;
    std::vector<uint8_t> masked;require(saveVBX(maskInput,masked,error),"mask fixture save");
    const size_t nodeStart=407+36*2,rootNode=nodeStart+128,childStart=nodeStart+192;
    write64(masked,rootNode+56,9);write64(masked,childStart+8,uint64_t(1)<<16);write64(masked,childStart+24,UINT64_MAX);
    masked.erase(masked.begin()+2,masked.begin()+50);masked[1]=0;write64(masked,6,14);
    VbxVolume expanded;if(!loadVBX(masked.data(),masked.size(),expanded,error))throw std::runtime_error(error);
    require(expanded.packed.children==maskInput.packed.children,"nonconsecutive bitmask expansion");require(expanded.packed.nodes==maskInput.packed.nodes,"normalized bitmask nodes");
    if(argc>1){VbxVolume asset;if(!loadVBXFile(argv[1],asset,error))throw std::runtime_error(error);
        require(asset.packed.levels[0].nodes.z==10284&&asset.packed.volume.hierarchy.x==2,"explosion topology");require(asset.atlas.size()==10496000,"explosion atlas dimensions");
        std::vector<uint8_t> saved;if(!saveVBX(asset,saved,error))throw std::runtime_error(error);VbxVolume roundtrip;if(!loadVBX(saved.data(),saved.size(),roundtrip,error))throw std::runtime_error(error);equal(asset,roundtrip);
        std::cout<<"explosion.vbx: 10284 leaves, bitmask conversion and exact scalar/transform roundtrip passed\n";
    }
    std::cout<<"GVDB standalone VBX roundtrip and corrupt-input tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

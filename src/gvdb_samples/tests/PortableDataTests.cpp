// Validation of real portable pool packing, including malformed serialized data.
// SPDX-License-Identifier: Apache-2.0
#include "PortableFixtures.h"
#include <iostream>
#include <stdexcept>
using namespace gvdb::slang;
static void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
int main() {
    try {
        auto fixture=fixtures::twoBrickVolume();
        auto source=fixtures::volumeSource(fixture);
        PackedVolume packed;
        std::string error;
        require(packVolume(source,packed,error),error.c_str());
        require(packed.nodes==fixture.nodes && packed.children==fixture.children,"packing changed serialized references");
        require(packed.levels.size()==2 && packed.levels[1].nodes.x==128 && packed.levels[1].children.w==2,"packed level offsets");
        require(packed.atlasMap.size()==2 && packed.atlasMap[1].x==2,"atlas map packing");
        const auto original=packed.nodes;
        const auto reject=[&](VolumeSource invalid,const char* message) {
            require(!packVolume(invalid,packed,error),message);
            require(!error.empty(),"missing failure diagnostic");
            require(packed.nodes==original,"failure changed output snapshot");
        };
        auto invalid=source;invalid.volume.hierarchy.y=1;reject(invalid,"accepted missing root");
        invalid=source;invalid.levels[0].nodes.count=uint64_t(UINT32_MAX)+1;reject(invalid,"accepted truncated node count");
        invalid=source;invalid.levels[0].nodes.count=UINT32_MAX;reject(invalid,"accepted overflowing pool extent");
        invalid=source;invalid.levels[1].children.stride=56;reject(invalid,"accepted undersized child grid");
        invalid=source;invalid.volume.atlasResolutionAndApron.w=2;reject(invalid,"accepted empty brick interior");
        invalid=source;invalid.atlasMapCount=1;reject(invalid,"accepted missing atlas mapping");
        invalid=source;invalid.levels[1].logDimension=31;reject(invalid,"accepted shift overflow");
        auto modified=fixture;
        modified.nodes[10]=makeReference(1,1,0).x;
        reject(fixtures::volumeSource(modified),"accepted parent with child-list group");
        modified=fixture;modified.children[0]=makeReference(0,0,99).x;
        reject(fixtures::volumeSource(modified),"accepted missing child node");
        modified=fixture;modified.nodes[12]=0;
        reject(fixtures::volumeSource(modified),"accepted leaf child list");
        modified=fixture;modified.atlasMap[1].leaf=2;
        reject(fixtures::volumeSource(modified),"accepted missing atlas leaf");
        modified=fixture;modified.nodes[20]=6;
        reject(fixtures::volumeSource(modified),"accepted misplaced atlas brick");
        // Padding is retained, rather than assuming every node pool has stride 64.
        std::vector<uint32_t> padded(60,0x12345678u);
        for(size_t i=0;i<3;++i)std::copy_n(fixture.nodes.data()+16*i,16,padded.data()+20*i);
        auto strided=source;
        strided.levels[0].nodes={padded.data(),2,80};
        strided.levels[1].nodes={padded.data()+40,1,80};
        require(packVolume(strided,packed,error),error.c_str());
        require(packed.nodes==padded && packed.levels[1].nodes.x==160,"padded pool packing");
        // Validate independent fixture expectations used by GPU tests.
        require(fixtures::exclusiveScan({3,1,4})==std::vector<uint32_t>({0,3,4}),"scan fixture");
        auto sorted=fixtures::stableSorted(fixtures::sortInput(257));
        for(size_t i=1;i<sorted.size();++i) {
            require(!keyLess(sorted[i].key,sorted[i-1].key),"sort fixture ordering");
            if(sorted[i].key.x==sorted[i-1].key.x && sorted[i].key.y==sorted[i-1].key.y)
                require(sorted[i].value>sorted[i-1].value,"sort fixture stability");
        }
        const auto reduction=fixtures::reduce({-2.f,std::numeric_limits<float>::quiet_NaN(),3.f});
        require(reduction.minimum==-2 && reduction.maximum==3 && reduction.sum==1 && reduction.count==2,"reduction fixture");
        std::cout<<"GVDB portable packing and malformed-input tests passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}

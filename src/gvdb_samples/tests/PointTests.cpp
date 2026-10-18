// SPDX-License-Identifier: Apache-2.0
#include "PointFixtures.h"
#include <iostream>
#include <stdexcept>
using namespace gvdb::slang;
static void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
int main(){try{
    auto keys=fixtures::uniqueBrickKeys(fixtures::brickKeyPositions(),{2,4});
    require(keys.size()==7,"unique key fixture count");
    require(keys.front().key.x==0&&keys.front().key.y==0&&keys.front().value==0,"stable first point key payload");
    require(keys.back().key.x==0&&keys.back().key.y==0x00ff0000u,"invalid position sentinel");
    bool largeValid=false;for(const auto& key:keys)if(key.key.y==(1u<<16|32768u))largeValid=true;
    require(largeValid,"coordinate range is evaluated independently for each level");
    fixtures::PointGatherFixture fixture;
    auto density=fixtures::gatheredValues(fixture,false);auto levelSet=fixtures::gatheredValues(fixture,true);
    require(std::abs(density[0]-(2.f-std::sqrt(.875f)))<1e-6f,"analytic particle distance");
    require(std::abs(levelSet[0]+density[0])<1e-6f,"density versus level set sign");
    auto accumulated=fixtures::gatheredValues(fixture,false,std::vector<float>(16,5));
    for(float v:accumulated)require(v==5,"gather accumulation preserves larger density");
    fixture.cells[1].points.y=0;auto empty=fixtures::gatheredValues(fixture,true);
    require(empty[2]==3,"empty subcell level set background");
    std::cout<<"Portable point fixture tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

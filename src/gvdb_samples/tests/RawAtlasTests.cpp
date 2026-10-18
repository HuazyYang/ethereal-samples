// Layout, packing and analytic expectations for raw-channel GPU fixtures.
// SPDX-License-Identifier: Apache-2.0
#include "RawAtlasFixtures.h"
#include <iostream>
#include <stdexcept>
using namespace gvdb::slang;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try {
    require(atlasStride(AtlasFormat::R8_UINT)==1&&atlasStride(AtlasFormat::RGB32_FLOAT)==12,"channel strides");
    require(atlasStorageBytes(AtlasFormat::R8_UINT,5)==8,"R8 storage padding");
    require(atlasStorageBytes(AtlasFormat::RGBA32_FLOAT,UINT32_MAX)==uint64_t(UINT32_MAX)*16,"64-bit storage extent");
    const AtlasFormat formats[]={AtlasFormat::R8_UINT,AtlasFormat::RGBA8_UINT,AtlasFormat::R32_FLOAT,AtlasFormat::RGB32_FLOAT,AtlasFormat::RGBA32_FLOAT};
    for(auto format:formats){
        auto original=fixtures::patternedRawAtlas(format);auto apron=fixtures::rawApron(original,format,0);
        require(fixtures::readRaw(apron,format,fixtures::fixtureIndex(3,1,1))==fixtures::readRaw(original,format,fixtures::fixtureIndex(5,1,1)),"right cross-brick apron");
        require(fixtures::readRaw(apron,format,fixtures::fixtureIndex(4,1,1))==fixtures::readRaw(original,format,fixtures::fixtureIndex(2,1,1)),"left cross-brick apron");
        require(fixtures::readRaw(apron,format,0)==std::array<float,4>{},"exterior boundary");
        require(fixtures::readRaw(apron,format,fixtures::fixtureIndex(6,2,2))==fixtures::readRaw(original,format,fixtures::fixtureIndex(6,2,2)),"interior preservation");
    }
    std::vector<uint8_t> seeds(128,0);seeds[fixtures::fixtureIndex(2,1,1)]=7;
    auto expanded=fixtures::byteExpansion(fixtures::rawApron(seeds,AtlasFormat::R8_UINT,0),7,19);
    require(expanded[fixtures::fixtureIndex(5,1,1)]==19,"byte expansion crosses sparse brick boundary");
    require(expanded[fixtures::fixtureIndex(2,1,1)]==7,"byte expansion preserves seed");
    require(expanded[fixtures::fixtureIndex(6,1,1)]==0,"byte expansion is one pass");
    std::vector<uint8_t> colors(128*4,0);colors[fixtures::fixtureIndex(1,1,1)*4]=200;
    colors[fixtures::fixtureIndex(1,1,1)*4+1]=100;
    auto color=fixtures::colorExpansion(colors,2,0);
    require(color[fixtures::fixtureIndex(1,1,1)*4]==255&&color[fixtures::fixtureIndex(1,1,1)*4+1]==127&&color[fixtures::fixtureIndex(1,1,1)*4+3]==1,"legacy integer color normalization");
    std::cout<<"Portable raw atlas fixture tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

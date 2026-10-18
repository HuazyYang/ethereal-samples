// Independent point-key and subcell-gather expectations for GPU verification.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PortableFixtures.h"
namespace gvdb::slang::fixtures {
inline std::vector<Float4> brickKeyPositions(){return {{.25f,.5f,.75f,1},{1.5f,1.5f,1.5f,1},{2.5f,.5f,.5f,1},{4.5f,3.5f,7.5f,1},{-1,0,0,1},{131072.f,0,0,1},{std::numeric_limits<float>::quiet_NaN(),0,0,1}};}
inline std::vector<KeyValue> uniqueBrickKeys(const std::vector<Float4>& positions,const std::vector<uint32_t>& ranges,Float4 translation={}) {
    std::vector<KeyValue> keys;
    for(size_t i=0;i<positions.size();++i)for(size_t level=0;level<ranges.size();++level) {
        const double coordinates[]={double(positions[i].x)+translation.x,double(positions[i].y)+translation.y,double(positions[i].z)+translation.z};
        uint64_t cell[3]{};bool valid=true;
        for(size_t axis=0;axis<3;++axis){const double c=std::floor(coordinates[axis]/ranges[level]);if(!std::isfinite(c)||c<0||c>65535)valid=false;else cell[axis]=uint64_t(c);}
        const uint64_t packed=valid?(uint64_t(level)<<48)|(cell[0]<<32)|(cell[1]<<16)|cell[2]:(uint64_t(255)<<48);
        keys.push_back({{uint32_t(packed),uint32_t(packed>>32)},uint32_t(i),0});
    }
    keys=stableSorted(std::move(keys));
    keys.erase(std::unique(keys.begin(),keys.end(),[](const KeyValue& a,const KeyValue& b){return a.key.x==b.key.x&&a.key.y==b.key.y;}),keys.end());
    return keys;
}
struct PointGatherFixture {
    UInt4 resolution{4,2,2,0};
    std::vector<GatherCell> cells={{{0,0,0,2},{0,0,0,0},{0,2,0,0}},{{2,0,0,2},{2,0,0,0},{0,2,0,0}}};
    std::vector<Float4> positions={{.25f,.5f,.75f,1},{2.75f,.5f,.25f,1}};
    std::vector<uint32_t> colors={0xff0000ffu,0xff00ff00u};
    float radius=2;
};
inline std::vector<float> gatheredValues(const PointGatherFixture& fixture,bool levelSet,const std::vector<float>& previous={}) {
    const size_t count=size_t(fixture.resolution.x)*fixture.resolution.y*fixture.resolution.z;
    std::vector<float> result=previous.empty()?std::vector<float>(count,levelSet?3.f:0.f):previous;
    for(const auto& cell:fixture.cells)for(int z=0;z<cell.worldOriginAndWidth.w;++z)for(int y=0;y<cell.worldOriginAndWidth.w;++y)for(int x=0;x<cell.worldOriginAndWidth.w;++x) {
        const size_t index=((cell.atlasOrigin.z+z)*fixture.resolution.y+cell.atlasOrigin.y+y)*fixture.resolution.x+cell.atlasOrigin.x+x;
        double value=result[index];
        for(uint32_t p=0;p<cell.points.y;++p){const auto point=fixture.positions[cell.points.x+p];
            const double dx=double(point.x)-(cell.worldOriginAndWidth.x+x),dy=double(point.y)-(cell.worldOriginAndWidth.y+y),dz=double(point.z)-(cell.worldOriginAndWidth.z+z);
            const double distance=std::sqrt(dx*dx+dy*dy+dz*dz);
            value=levelSet?std::min(value,distance-fixture.radius):std::max(value,fixture.radius-distance);
        }
        result[index]=float(value);
    }
    return result;
}
} // namespace gvdb::slang::fixtures

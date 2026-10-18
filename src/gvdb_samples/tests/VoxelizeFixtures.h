// Analytic closed-box fixtures for conservative triangle voxelization.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PortableFixtures.h"
namespace gvdb::slang::fixtures {
struct VoxelizeFixture {
    std::vector<Float4> vertices={{1.25f,1.25f,1.25f,1},{4.75f,1.25f,1.25f,1},{4.75f,4.75f,1.25f,1},{1.25f,4.75f,1.25f,1},
        {1.25f,1.25f,4.75f,1},{4.75f,1.25f,4.75f,1},{4.75f,4.75f,4.75f,1},{1.25f,4.75f,4.75f,1}};
    std::vector<uint32_t> indices={0,2,1,0,3,2,4,5,6,4,6,7,0,1,5,0,5,4,3,7,6,3,6,2,0,4,7,0,7,3,1,2,6,1,6,5};
    VoxelizeParameters parameters{};
    VoxelizeFixture(){parameters.boundsMaximum={6,6,6,0};parameters.resolution={6,6,6,0};parameters.values={9,3,0,0};
        parameters.transformRows[0]={1,0,0,0};parameters.transformRows[1]={0,1,0,0};parameters.transformRows[2]={0,0,1,0};}
};
// The box lies strictly inside voxel columns 1..4: boundary cells touch the
// surface, the 2x2x2 central cells are inside, and all remaining cells outside.
inline std::vector<float> voxelizedBox(){
    std::vector<float> result(216,0);
    for(uint32_t z=1;z<=4;++z)for(uint32_t y=1;y<=4;++y)for(uint32_t x=1;x<=4;++x)
        result[(z*6+y)*6+x]=(x==1||x==4||y==1||y==4||z==1||z==4)?9.f:3.f;
    return result;
}
} // namespace gvdb::slang::fixtures

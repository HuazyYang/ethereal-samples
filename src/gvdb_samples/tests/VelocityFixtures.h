// Analytic affine MAC field; catches face staggering and sparse seam mistakes.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PortableFixtures.h"
#include <array>
namespace gvdb::slang::fixtures {
inline Float4 affineVelocity(Float4 position){return {2*position.x+3*position.y-position.z+1,-position.x+4*position.y+2*position.z-2,position.x-position.y+5*position.z+.5f,0};}
inline std::array<std::vector<float>,3> macVelocityFields(){
    std::array<std::vector<float>,3> fields;for(auto& field:fields)field.resize(128);
    for(int z=0;z<4;++z)for(int y=0;y<4;++y)for(int x=0;x<8;++x){
        const float wx=float(x<4?x-1:x-3),wy=float(y-1),wz=float(z-1);const size_t index=size_t((z*4+y)*8+x);
        fields[0][index]=affineVelocity({wx,wy+.5f,wz+.5f,0}).x;
        fields[1][index]=affineVelocity({wx+.5f,wy,wz+.5f,0}).y;
        fields[2][index]=affineVelocity({wx+.5f,wy+.5f,wz,0}).z;
    }
    return fields;
}
inline std::vector<Float4> velocityPositions(){return {{1.25f,.75f,1,3},{2.1f,1,1,7},{-1,0,0,9}};}
inline Float4 midpointPosition(Float4 position,float dt){
    const auto first=affineVelocity(position);Float4 middle{position.x+first.x*dt*.5f,position.y+first.y*dt*.5f,position.z+first.z*dt*.5f,position.w};
    const auto second=affineVelocity(middle);return {position.x+second.x*dt,position.y+second.y*dt,position.z+second.z*dt,position.w};
}
} // namespace gvdb::slang::fixtures

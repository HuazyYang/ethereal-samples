// Independent CPU expectations for raw-format atlas GPU tests.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PortableFixtures.h"
#include <array>
namespace gvdb::slang::fixtures {
inline std::array<float,4> readRaw(const std::vector<uint8_t>& bytes,AtlasFormat format,size_t voxel) {
    std::array<float,4> result{};const size_t stride=atlasStride(format);
    if(format==AtlasFormat::R8_UINT)result[0]=float(bytes.at(voxel));
    else if(format==AtlasFormat::RGBA8_UINT)for(size_t c=0;c<4;++c)result[c]=float(bytes.at(voxel*4+c));
    else for(size_t c=0;c<stride/4;++c)std::memcpy(&result[c],bytes.data()+voxel*stride+c*4,4);
    return result;
}
inline void writeRaw(std::vector<uint8_t>& bytes,AtlasFormat format,size_t voxel,const std::array<float,4>& value) {
    const size_t stride=atlasStride(format);
    if(format==AtlasFormat::R8_UINT)bytes.at(voxel)=uint8_t(int(value[0]));
    else if(format==AtlasFormat::RGBA8_UINT)for(size_t c=0;c<4;++c)bytes.at(voxel*4+c)=uint8_t(int(value[c]));
    else for(size_t c=0;c<stride/4;++c)std::memcpy(bytes.data()+voxel*stride+c*4,&value[c],4);
}
inline bool fixtureInterior(int x,int y,int z){return (x%4==1||x%4==2)&&(y==1||y==2)&&(z==1||z==2);}
inline size_t fixtureIndex(int x,int y,int z){return size_t((z*4+y)*8+x);}
// Unique finite values per component and location catch byte-order/stride mistakes.
inline std::vector<uint8_t> patternedRawAtlas(AtlasFormat format) {
    std::vector<uint8_t> bytes(128*atlasStride(format));
    for(int z=0;z<4;++z)for(int y=0;y<4;++y)for(int x=0;x<8;++x) {
        std::array<float,4> value{};
        for(size_t c=0;c<4;++c)value[c]=float((fixtureIndex(x,y,z)*13+c*31+7)%251);
        if(uint32_t(format)>=2)for(float& v:value)v=v*.25f-30.f;
        writeRaw(bytes,format,fixtureIndex(x,y,z),value);
    }
    return bytes;
}
// This fixture's two bricks cover [0,4)x[0,2)x[0,2). Compute the expected
// neighbor directly in world coordinates, independently of serialized topology.
inline std::vector<uint8_t> rawApron(const std::vector<uint8_t>& input,AtlasFormat format,float boundary) {
    auto output=input;
    for(int z=0;z<4;++z)for(int y=0;y<4;++y)for(int x=0;x<8;++x)if(!fixtureInterior(x,y,z)) {
        const int wx=x<4?x-1:x-3,wy=y-1,wz=z-1;
        std::array<float,4> value={boundary,boundary,boundary,boundary};
        if(wx>=0&&wx<4&&wy>=0&&wy<2&&wz>=0&&wz<2) {
            const int ax=wx<2?wx+1:wx+3;
            value=readRaw(input,format,fixtureIndex(ax,wy+1,wz+1));
        }
        writeRaw(output,format,fixtureIndex(x,y,z),value);
    }
    return output;
}
inline std::vector<uint8_t> rawSmooth(const std::vector<uint8_t>& source,AtlasFormat format,float center,float bias) {
    auto output=source;
    const int delta[6][3]={{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
    for(int z=1;z<=2;++z)for(int y=1;y<=2;++y)for(int x=0;x<8;++x)if(fixtureInterior(x,y,z)) {
        auto value=readRaw(source,format,fixtureIndex(x,y,z));
        for(float& v:value)v*=center;
        for(const auto& d:delta) {auto n=readRaw(source,format,fixtureIndex(x+d[0],y+d[1],z+d[2]));for(size_t c=0;c<4;++c)value[c]+=n[c];}
        for(float& v:value)v=v/(center+6)+bias;
        writeRaw(output,format,fixtureIndex(x,y,z),value);
    }
    return output;
}
inline std::vector<uint8_t> byteExpansion(const std::vector<uint8_t>& source,uint8_t from,uint8_t to) {
    auto output=source;
    const int delta[6][3]={{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
    for(int z=1;z<=2;++z)for(int y=1;y<=2;++y)for(int x=0;x<8;++x)if(fixtureInterior(x,y,z)&&source[fixtureIndex(x,y,z)]==0)
        for(const auto& d:delta)if(source[fixtureIndex(x+d[0],y+d[1],z+d[2])]==from)output[fixtureIndex(x,y,z)]=to;
    return output;
}
inline std::vector<uint8_t> colorExpansion(const std::vector<uint8_t>& source,float center,float neighbor) {
    auto output=source;
    const int delta[6][3]={{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
    for(int z=1;z<=2;++z)for(int y=1;y<=2;++y)for(int x=0;x<8;++x)if(fixtureInterior(x,y,z)) {
        const size_t i=fixtureIndex(x,y,z)*4;
        int channels[3]={int(source[i]*center),int(source[i+1]*center),int(source[i+2]*center)};
        for(const auto& d:delta){const size_t j=fixtureIndex(x+d[0],y+d[1],z+d[2])*4;for(size_t c=0;c<3;++c)channels[c]+=int(source[j+c]*neighbor);}
        const int maximum=std::max(channels[0],std::max(channels[1],channels[2]));
        for(size_t c=0;c<3;++c)output[i+c]=uint8_t(maximum>255?channels[c]*255/maximum:channels[c]);
        output[i+3]=1;
    }
    return output;
}
} // namespace gvdb::slang::fixtures

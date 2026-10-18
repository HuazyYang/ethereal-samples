// SPDX-License-Identifier: Apache-2.0
#include "GVDBSlangAtlas.h"
#include "ComputeRuntime.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>
using nvrhi::BindingSetItem;
namespace gvdb::slang {
namespace {
void finite(float v){if(!std::isfinite(v))throw std::invalid_argument("GVDB atlas parameters must be finite");}
void byteRange(float v){finite(v);if(v<0||v>255)throw std::invalid_argument("GVDB byte value is outside [0,255]");}
}
SlangAtlas::SlangAtlas(ethereal::slang::ComputeRuntime& runtime,const PackedVolume& volume,
    AtlasFormat format,const void* initialData,size_t bytes):m_runtime(&runtime),m_volume(volume.volume),m_format(format) {
    const auto res=m_volume.atlasResolutionAndApron;
    if(!res.x||!res.y||!res.z||uint64_t(res.x)*res.y>UINT32_MAX||uint64_t(res.x)*res.y*res.z>UINT32_MAX)
        throw std::invalid_argument("GVDB atlas voxel count exceeds the portable range");
    m_voxels=res.x*res.y*res.z;
    const uint32_t stride=atlasStride(format);
    const uint64_t storage=atlasStorageBytes(format,m_voxels);
    if(!stride||storage>uint64_t(UINT32_MAX)+1||bytes!=uint64_t(m_voxels)*stride||!initialData||
        volume.nodes.empty()||volume.levels.empty()||volume.atlasMap.empty())
        throw std::invalid_argument("GVDB atlas upload requires a packed volume and a complete supported channel");
    m_bytes=bytes;m_storage=size_t(storage);m_work=format==AtlasFormat::R8_UINT?m_voxels/4+uint32_t(m_voxels%4!=0):m_voxels;
    std::vector<uint8_t> padded(m_storage,0);std::memcpy(padded.data(),initialData,m_bytes);
    m_atlas=runtime.buffer(m_storage,0,padded.data(),true);m_scratch=runtime.buffer(m_storage,0,nullptr,true);
    m_nodes=runtime.buffer(volume.nodes.size()*4,0,volume.nodes.data(),true);
    const uint32_t emptyChildren[2]={UINT32_MAX,UINT32_MAX};
    m_children=runtime.buffer(volume.children.empty()?sizeof(emptyChildren):volume.children.size()*4,0,
        volume.children.empty()?emptyChildren:volume.children.data(),true);
    m_levels=runtime.buffer(volume.levels.size()*sizeof(LevelInfo),sizeof(LevelInfo),volume.levels.data());
    m_map=runtime.buffer(volume.atlasMap.size()*sizeof(AtlasNode),sizeof(AtlasNode),volume.atlasMap.data());
}
void SlangAtlas::operate(const char* entry,Float4 value) {
    RawOperatorParameters p{m_volume,value,{uint32_t(m_format),atlasStride(m_format),m_voxels,m_work}};
    const uint32_t groups=m_work/256+uint32_t(m_work%256!=0);
    m_runtime->dispatch("GVDBRawOperators",entry,&p,sizeof(p),{
        BindingSetItem::RawBuffer_SRV(0,m_atlas),BindingSetItem::StructuredBuffer_SRV(1,m_map),
        BindingSetItem::RawBuffer_SRV(2,m_nodes),BindingSetItem::RawBuffer_SRV(3,m_children),
        BindingSetItem::StructuredBuffer_SRV(4,m_levels),BindingSetItem::RawBuffer_UAV(0,m_scratch)},
        std::min(groups,65535u),groups/65535u+uint32_t(groups%65535u!=0));
    std::swap(m_atlas,m_scratch);
}
void SlangAtlas::clear(){operate("clearRawAtlas",{});}
void SlangAtlas::fill(Float4 value) {
    finite(value.x);finite(value.y);finite(value.z);finite(value.w);
    if(m_format==AtlasFormat::R8_UINT)byteRange(value.x);
    if(m_format==AtlasFormat::RGBA8_UINT&&(value.x<0||value.x>1||value.y<0||value.y>1||value.z<0||value.z>1))
        throw std::invalid_argument("GVDB color fill requires normalized RGB components");
    operate("fillRawAtlas",value);
}
void SlangAtlas::updateApron(float boundary) {
    finite(boundary);if(m_format==AtlasFormat::R8_UINT||m_format==AtlasFormat::RGBA8_UINT)byteRange(boundary);
    operate("updateRawApron",{boundary,0,0,0});
}
void SlangAtlas::smooth(float centerWeight,float bias,uint32_t iterations,float boundary) {
    finite(centerWeight);finite(bias);finite(boundary);
    if(uint32_t(m_format)<uint32_t(AtlasFormat::R32_FLOAT)||centerWeight==-6)
        throw std::invalid_argument("GVDB smooth requires a float channel and a nonzero denominator");
    for(uint32_t i=0;i<iterations;++i){updateApron(boundary);operate("smoothRawAtlas",{centerWeight,bias,0,0});}
    if(iterations)updateApron(boundary);
}
void SlangAtlas::expandByte(uint8_t from,uint8_t to,uint32_t iterations,uint8_t boundary) {
    if(m_format!=AtlasFormat::R8_UINT)throw std::invalid_argument("GVDB byte expansion requires R8_UINT");
    for(uint32_t i=0;i<iterations;++i){updateApron(float(boundary));operate("expandByteAtlas",{float(from),float(to),0,0});}
    if(iterations)updateApron(float(boundary));
}
void SlangAtlas::expandColor(float centerWeight,float neighborWeight,uint32_t iterations,uint8_t boundary) {
    finite(centerWeight);finite(neighborWeight);
    // Bounds keep both accumulation and the legacy integer *255 normalization in range.
    if(m_format!=AtlasFormat::RGBA8_UINT||centerWeight<0||neighborWeight<0||double(centerWeight)+6*double(neighborWeight)>32767)
        throw std::invalid_argument("GVDB color expansion requires RGBA8_UINT and nonnegative bounded weights");
    for(uint32_t i=0;i<iterations;++i){updateApron(float(boundary));operate("expandColorAtlas",{centerWeight,neighborWeight,0,0});}
    if(iterations)updateApron(float(boundary));
}
std::vector<uint8_t> SlangAtlas::read() {
    std::vector<uint8_t> result(m_storage);m_runtime->read(m_atlas,result.data(),m_storage);result.resize(m_bytes);return result;
}
} // namespace gvdb::slang

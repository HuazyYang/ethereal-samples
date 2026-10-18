// Deterministic fixtures and independent CPU expectations for GPU harnesses.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../gvdb/slang/GVDBPortableData.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

namespace gvdb::slang::fixtures {
struct SparseVolume {
    VolumeInfo volume{};
    std::vector<LevelInfo> levels;
    std::vector<uint32_t> nodes;
    std::vector<uint32_t> children;
    std::vector<AtlasNode> atlasMap;
    std::vector<float> atlas;
};
inline uint32_t floatBits(float value) { uint32_t bits; std::memcpy(&bits, &value, 4); return bits; }
inline SparseVolume twoBrickVolume(float density = 1.f) {
    SparseVolume result;
    result.volume.atlasResolutionAndApron = {8,4,4,1};
    result.volume.atlasBrickCountAndWidth = {2,1,1,4};
    result.volume.hierarchy = {1,0,2,0};
    result.volume.boundsMin = {0,0,0,0};
    result.volume.boundsMax = {4,2,2,0};
    result.levels.resize(2);
    result.levels[0] = {{0,64,2,1}, {0,0,0,2}, {1,1,1,0}, {2,2,2,0}};
    result.levels[1] = {{128,64,1,1}, {0,64,1,2}, {2,2,2,0}, {4,4,4,0}};
    result.nodes.resize(48,0);
    for (uint32_t i=0;i<3;++i) {
        uint32_t* words = result.nodes.data()+16*i;
        words[0] = 0x100u | (i==2 ? 1u : 0u);
        words[1] = i==1 ? 2 : 0;
        words[4] = i==1 ? 5 : 1; words[5] = words[6] = 1;
        words[7] = words[8] = words[9] = floatBits(density);
        const UInt2 parent = i==2 ? UInt2{UINT32_MAX,UINT32_MAX} : makeReference(0,1,0);
        const UInt2 list = i==2 ? makeReference(1,1,0) : UInt2{UINT32_MAX,UINT32_MAX};
        words[10] = parent.x; words[11] = parent.y;
        words[12] = list.x; words[13] = list.y;
    }
    result.children.resize(16,UINT32_MAX);
    for (uint32_t i=0;i<2;++i) {
        const auto ref = makeReference(0,0,i);
        result.children[2*i] = ref.x; result.children[2*i+1] = ref.y;
    }
    result.atlasMap = {{0,0,0,0},{2,0,0,1}};
    result.atlas.resize(8*4*4,density);
    return result;
}
inline VolumeSource volumeSource(const SparseVolume& fixture) {
    VolumeSource source;
    source.volume=fixture.volume;
    for (const auto& level:fixture.levels) {
        const auto* nodes=reinterpret_cast<const uint8_t*>(fixture.nodes.data())+level.nodes.x;
        const auto* children=reinterpret_cast<const uint8_t*>(fixture.children.data())+level.children.x;
        source.levels.push_back({{nodes,level.nodes.z,level.nodes.y},{children,level.children.z,level.children.y},level.nodes.w});
    }
    source.atlasMap=fixture.atlasMap.data();source.atlasMapCount=fixture.atlasMap.size();
    return source;
}
inline std::vector<uint32_t> scanInput(uint32_t count) {
    std::vector<uint32_t> result(count);
    for (uint32_t i=0;i<count;++i) result[i] = (i*17u+3u)%11u;
    return result;
}
inline std::vector<uint32_t> exclusiveScan(const std::vector<uint32_t>& values) {
    std::vector<uint32_t> result(values.size());
    std::exclusive_scan(values.begin(),values.end(),result.begin(),uint32_t(0));
    return result;
}
inline std::vector<KeyValue> sortInput(uint32_t count) {
    std::mt19937 generator(0x6f766462u);
    std::vector<KeyValue> result(count);
    for (uint32_t i=0;i<count;++i) {
        result[i] = {{generator(),generator()},i,0};
        if (i%7==0) result[i].key = {0xffffffffu,0};
        if (i%11==0) result[i].key = {0,1};
        if (i%13==0) result[i].key = {0xffffffffu,0xffffffffu};
        if (i%17==0) result[i].key = {0,0};
    }
    return result;
}
inline std::vector<KeyValue> stableSorted(std::vector<KeyValue> values) {
    std::stable_sort(values.begin(),values.end(),[](const KeyValue& a,const KeyValue& b) {
        const uint64_t x = (uint64_t(a.key.y)<<32) | a.key.x;
        const uint64_t y = (uint64_t(b.key.y)<<32) | b.key.x;
        return x < y;
    });
    return values;
}
inline Reduction reduce(const std::vector<float>& input) {
    Reduction result{std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),0,0};
    double sum=0;
    for (float value:input) if (!std::isnan(value)) {
        result.minimum=std::min(result.minimum,value);
        result.maximum=std::max(result.maximum,value);
        sum+=value; ++result.count;
    }
    result.sum=float(sum);
    return result;
}
inline OperatorParameters operators(const SparseVolume& fixture) {
    OperatorParameters result{};
    result.volume=fixture.volume;
    result.value={2,0,1,0};
    result.sourceResolution={8,4,4,0};
    result.transformRows[0]={1,0,0,0};
    result.transformRows[1]={0,1,0,0};
    result.transformRows[2]={0,0,1,0};
    result.remap={0,1,0,1};
    return result;
}
inline std::vector<float> smooth(const SparseVolume& fixture, float centerWeight, float offset) {
    auto result=fixture.atlas;
    const auto resolution=fixture.volume.atlasResolutionAndApron;
    const uint32_t width=fixture.volume.atlasBrickCountAndWidth.w;
    const uint32_t apron=resolution.w;
    const auto load=[&](uint32_t x,uint32_t y,uint32_t z){return fixture.atlas[(z*resolution.y+y)*resolution.x+x];};
    for(uint32_t z=0;z<resolution.z;++z) for(uint32_t y=0;y<resolution.y;++y) for(uint32_t x=0;x<resolution.x;++x) {
        if (x%width<apron || x%width>=width-apron || y%width<apron || y%width>=width-apron || z%width<apron || z%width>=width-apron) continue;
        result[(z*resolution.y+y)*resolution.x+x]=(centerWeight*load(x,y,z)+load(x-1,y,z)+load(x+1,y,z)+load(x,y-1,z)+load(x,y+1,z)+load(x,y,z-1)+load(x,y,z+1))/(centerWeight+6)+offset;
    }
    return result;
}
inline SphParameters sphParameters() {
    SphParameters result{};
    result.gridMinimumAndCellWidth={0,0,0,2};
    result.gridResolutionAndCount={3,3,3,2};
    result.physical={2,1,1,1};
    result.kernels={1,1,0,1};
    result.boundaryMinimum={-100,-100,-100,0};
    result.boundaryMaximum={100,100,100,0};
    result.boundary={0.1f,10,0,0};
    result.integration={0,0.01f,100,100};
    return result;
}
// Two particles one unit apart: rho = (h^2-r^2)^3 = 27; pressures
// are 26 each, forces are equal/opposite 26/729 along their separation.
inline std::vector<Float4> sphPositions() { return {{1,1,1,0},{2,1,1,0}}; }
inline float sphExpectedDensity() { return 27.f; }
inline float sphExpectedForce() { return 26.f/729.f; }
inline RaycastParameters raycastParameters(const SparseVolume& fixture) {
    RaycastParameters result{};
    result.volume=fixture.volume;
    result.image={1,1,2,64};
    result.rayOrigin={-1,1,1,0};
    result.direction00={1,0,0,0};
    result.march={0.25f,0,100,1};
    result.valueRange={0,1,0.5f,1};
    result.background={0,0,0,0};
    result.light={-10,1,1,0.1f};
    return result;
}
// Constant transfer {1,.5,.25,.5}, ray length 4: analytic Beer-Lambert result.
inline Float4 raycastExpected() {
    float opacity=1-std::exp(-2.f);
    return {opacity,opacity*0.5f,opacity*0.25f,opacity};
}
} // namespace gvdb::slang::fixtures

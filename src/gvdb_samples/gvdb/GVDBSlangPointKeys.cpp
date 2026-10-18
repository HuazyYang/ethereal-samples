// SPDX-License-Identifier: Apache-2.0
#include "GVDBSlangPointKeys.h"
#include "ComputeRuntime.h"
#include <cmath>
#include <numeric>
#include <stdexcept>
using nvrhi::BindingSetItem;
namespace gvdb::slang {
SlangBrickKeys discoverBrickKeys(ethereal::slang::ComputeRuntime& runtime,nvrhi::IBuffer* positions,
    uint32_t pointCount,const std::vector<uint32_t>& levelRanges,Float4 translation) {
    if(levelRanges.empty()||levelRanges.size()>kMaxLevels||!std::isfinite(translation.x)||!std::isfinite(translation.y)||!std::isfinite(translation.z))
        throw std::invalid_argument("GVDB point keys require valid levels and finite translation");
    for(uint32_t range:levelRanges)if(!range)throw std::invalid_argument("GVDB point key level extent is zero");
    SlangBrickKeys result;result.levelCounts.resize(levelRanges.size(),0);
    if(!pointCount)return result;
    const uint64_t total=uint64_t(pointCount)*levelRanges.size();
    if(!positions||positions->getDesc().byteSize<uint64_t(pointCount)*sizeof(Float4)||total>65535u*256u)
        throw std::invalid_argument("GVDB point key input exceeds its buffer or the scan dispatch limit");
    PointKeyParameters p{{pointCount,uint32_t(levelRanges.size()),uint32_t(total),0},translation,{}};
    auto ranges=runtime.buffer(levelRanges.size()*4,4,levelRanges.data());
    auto keys=runtime.buffer(size_t(total)*sizeof(KeyValue),sizeof(KeyValue));
    auto markers=runtime.buffer(size_t(total)*4,4);
    auto counts=runtime.buffer((levelRanges.size()+1)*4,4);
    const uint32_t groups=uint32_t(total)/256+uint32_t(total%256!=0);
    runtime.dispatch("GVDBPointKeys","calculateBrickKeys",&p,sizeof(p),{
        BindingSetItem::StructuredBuffer_SRV(0,positions),BindingSetItem::StructuredBuffer_SRV(1,ranges),
        BindingSetItem::StructuredBuffer_UAV(0,keys)},groups);
    keys=runtime.radixSort(keys,uint32_t(total));
    runtime.dispatch("GVDBPointKeys","clearUniqueCounts",&p,sizeof(p),{BindingSetItem::StructuredBuffer_UAV(2,counts)},1);
    runtime.dispatch("GVDBPointKeys","markUniqueKeys",&p,sizeof(p),{
        BindingSetItem::StructuredBuffer_SRV(2,keys),BindingSetItem::StructuredBuffer_UAV(1,markers),
        BindingSetItem::StructuredBuffer_UAV(2,counts)},groups);
    auto offsets=runtime.exclusiveScan(markers,uint32_t(total));
    std::vector<uint32_t> hostCounts(levelRanges.size()+1);runtime.read(counts,hostCounts.data(),hostCounts.size()*4);
    result.count=hostCounts[0];result.levelCounts.assign(hostCounts.begin()+1,hostCounts.end());
    const uint32_t valid=std::accumulate(result.levelCounts.begin(),result.levelCounts.end(),0u);
    result.hasInvalidPositions=result.count!=valid;
    result.keys=runtime.buffer(size_t(result.count)*sizeof(KeyValue),sizeof(KeyValue));
    runtime.dispatch("GVDBPointKeys","compactUniqueKeys",&p,sizeof(p),{
        BindingSetItem::StructuredBuffer_SRV(2,keys),BindingSetItem::StructuredBuffer_SRV(3,markers),
        BindingSetItem::StructuredBuffer_SRV(4,offsets),BindingSetItem::StructuredBuffer_UAV(0,result.keys)},groups);
    return result;
}
} // namespace gvdb::slang

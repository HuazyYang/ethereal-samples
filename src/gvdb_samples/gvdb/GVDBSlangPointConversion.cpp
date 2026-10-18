// SPDX-License-Identifier: Apache-2.0
#include "GVDBSlangPointConversion.h"
#include "ComputeRuntime.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
using nvrhi::BindingSetItem;
namespace gvdb::slang {
namespace {
void finite(Float4 v){if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z)||!std::isfinite(v.w))throw std::invalid_argument("GVDB point conversion parameters must be finite");}
void launch(ethereal::slang::ComputeRuntime& runtime,const char* entry,const PointConversionParameters& p,const std::vector<BindingSetItem>& bindings,uint32_t work){
    const uint32_t groups=work/256+uint32_t(work%256!=0);runtime.dispatch("GVDBPointConversion",entry,&p,sizeof(p),bindings,std::min(groups,65535u),groups/65535u+uint32_t(groups%65535u!=0));
}
}
nvrhi::BufferHandle convertPackedPoints(ethereal::slang::ComputeRuntime& runtime,nvrhi::IBuffer* source,PointConversionParameters p){
    finite(p.minimum);finite(p.delta);finite(p.scale);finite(p.translation);
    const uint32_t count=p.source.x,bytes=p.source.y,offset=p.source.z,stride=p.source.w;
    if(bytes!=1&&bytes!=2&&bytes!=4&&bytes!=8)throw std::invalid_argument("GVDB point component width must be 1,2,4,or8 bytes");
    if(stride<3*bytes||stride%bytes||offset%bytes||count>UINT32_MAX/16)throw std::invalid_argument("GVDB point layout is misaligned or too large");
    if(!count)return {};
    const uint64_t extent=uint64_t(count-1)*stride+offset+3*bytes;
    if(!source||!source->getDesc().canHaveRawViews||extent>uint64_t(UINT32_MAX)+1||((extent+3)&~uint64_t(3))>source->getDesc().byteSize)
        throw std::invalid_argument("GVDB point input exceeds its padded raw buffer");
    auto output=runtime.buffer(size_t(count)*16,16);
    launch(runtime,"convertPointPositions",p,{BindingSetItem::RawBuffer_SRV(0,source),BindingSetItem::StructuredBuffer_UAV(0,output)},count);return output;
}
nvrhi::BufferHandle quantizePointsU16(ethereal::slang::ComputeRuntime& runtime,nvrhi::IBuffer* positions,uint32_t count,Float4 minimum,Float4 range){
    finite(minimum);finite(range);if(range.x<=0||range.y<=0||range.z<=0||count>UINT32_MAX/16)throw std::invalid_argument("GVDB point quantization range or count is invalid");
    if(!count)return {};
    if(!positions||positions->getDesc().structStride!=16||positions->getDesc().byteSize<uint64_t(count)*16)
        throw std::invalid_argument("GVDB quantization requires a complete float4 position buffer");
    PointConversionParameters p{{count,0,0,0},minimum,range,{1,1,1,1},{}};
    const size_t bytes=(size_t(count)*6+3)&~size_t(3);auto output=runtime.buffer(bytes,0,nullptr,true);
    launch(runtime,"quantizePointPositions",p,{BindingSetItem::StructuredBuffer_SRV(1,positions),BindingSetItem::RawBuffer_UAV(1,output)},uint32_t(bytes/4));return output;
}
} // namespace gvdb::slang

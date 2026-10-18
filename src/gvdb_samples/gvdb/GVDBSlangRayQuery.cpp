// SPDX-License-Identifier: Apache-2.0
#include "GVDBSlangRayQuery.h"
#include "ComputeRuntime.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace gvdb::slang {
RayQueryOutput queryVolumeRays(ethereal::slang::ComputeRuntime& runtime, const SlangVolume& volume,
                               nvrhi::IBuffer* origins, nvrhi::IBuffer* directions, uint32_t count,
                               RayQueryOptions options) {
    if (!origins || !directions || origins->getDesc().structStride != 16 ||
        directions->getDesc().structStride != 16 || origins->getDesc().byteSize < uint64_t(count) * 16 ||
        directions->getDesc().byteSize < uint64_t(count) * 16 || count > UINT32_MAX / 16 ||
        !std::isfinite(options.step) || options.step <= 0 || !std::isfinite(options.threshold) ||
        !std::isfinite(options.epsilon) || options.epsilon < 0 || !std::isfinite(options.bias) ||
        !options.maxLeafSteps || !options.maxLeafVisits)
        throw std::invalid_argument("Sparse ray-query buffers or sampling parameters are invalid");
    struct Parameters {
        VolumeInfo volume;
        UInt4 counts;
        Float4 sampling;
    };
    Parameters p{
        volume.info(),
        {count,        options.maxLeafSteps, options.maxLeafVisits, 0           },
        {options.step, options.threshold,    options.epsilon,       options.bias}
    };
    RayQueryOutput result{runtime.buffer(size_t(std::max(1u, count)) * 16, 16),
                          runtime.buffer(size_t(std::max(1u, count)) * 16, 16), count};
    if (count) {
        using B = nvrhi::BindingSetItem;
        const uint32_t groups = (count + 255) / 256;
        runtime.dispatch("GVDBRayQuery", "queryVolumeRays", &p, sizeof(p),
                         {B::StructuredBuffer_SRV(0, volume.atlas()), B::RawBuffer_SRV(1, volume.nodes()),
                          B::RawBuffer_SRV(2, volume.children()), B::StructuredBuffer_SRV(3, volume.levels()),
                          B::StructuredBuffer_SRV(4, origins), B::StructuredBuffer_SRV(5, directions),
                          B::StructuredBuffer_UAV(0, result.hits), B::StructuredBuffer_UAV(1, result.normals)},
                         std::min(groups, 65535u), (groups + 65534) / 65535);
    }
    return result;
}
}  // namespace gvdb::slang

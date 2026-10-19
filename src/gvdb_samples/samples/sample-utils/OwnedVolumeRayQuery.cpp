// SPDX-License-Identifier: Apache-2.0
#include "OwnedVolumeRayQuery.h"
#include "OwnedVolumeRawViews.h"
#include "ComputeRuntime.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace gvdb::sample {
using namespace slang;
RayQueryOutput queryOwnedVolumeRays(ethereal::slang::ComputeRuntime& runtime, portable::SnapshotHandle snapshot,
                                    nvrhi::IBuffer* origins, nvrhi::IBuffer* directions, uint32_t count,
                                    RayQueryOptions options, uint32_t scalarChannel) {
    OwnedVolumeRawViews volume(runtime.device(), std::move(snapshot), scalarChannel);
    if (!origins || !directions || origins->getDesc().structStride != 16 ||
        directions->getDesc().structStride != 16 || origins->getDesc().byteSize < uint64_t(count) * 16 ||
        directions->getDesc().byteSize < uint64_t(count) * 16 || count > UINT32_MAX / 16 ||
        !std::isfinite(options.step) || options.step <= 0 || !std::isfinite(options.threshold) ||
        !std::isfinite(options.epsilon) || options.epsilon < 0 || !std::isfinite(options.bias) ||
        !options.maxLeafSteps || !options.maxLeafVisits)
        throw std::invalid_argument("Sparse ray-query buffers or sampling parameters are invalid");
    nvrhi::BufferHandle emptyChildren;
    if (!volume.children.buffer) {
        const uint32_t missing[2] = {UINT32_MAX, UINT32_MAX};
        emptyChildren = runtime.buffer(8, 0, missing, true);
        volume.children = {emptyChildren, 0, 8};
    }
    struct Parameters {
        VolumeInfo volume;
        UInt4 counts;
        Float4 sampling;
    };
    Parameters p{
        volume.info.volume,
        {count,        options.maxLeafSteps, options.maxLeafVisits, 0           },
        {options.step, options.threshold,    options.epsilon,       options.bias}
    };
    RayQueryOutput result{runtime.buffer(size_t(std::max(1u, count)) * 16, 16),
                          runtime.buffer(size_t(std::max(1u, count)) * 16, 16), count};
    if (count) {
        using B = nvrhi::BindingSetItem;
        const uint32_t groups = (count + 255) / 256;
        runtime.dispatch(
            "GVDBRawRayQuery", "queryVolumeRays", &p, sizeof(p),
            {B::RawBuffer_SRV(0, volume.scalar.raw.buffer, OwnedVolumeRawViews::range(volume.scalar.raw)),
             B::RawBuffer_SRV(1, volume.nodes.buffer, OwnedVolumeRawViews::range(volume.nodes)),
             B::RawBuffer_SRV(2, volume.children.buffer, OwnedVolumeRawViews::range(volume.children)),
             B::StructuredBuffer_SRV(3, volume.levels.buffer, nvrhi::Format::UNKNOWN,
                                     OwnedVolumeRawViews::range(volume.levels)),
             B::StructuredBuffer_SRV(4, origins), B::StructuredBuffer_SRV(5, directions),
             B::StructuredBuffer_UAV(0, result.hits), B::StructuredBuffer_UAV(1, result.normals)},
            std::min(groups, 65535u), (groups + 65534) / 65535);
    }
    return result;
}
}  // namespace gvdb::sample

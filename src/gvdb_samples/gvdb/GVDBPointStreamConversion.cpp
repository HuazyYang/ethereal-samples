// SPDX-License-Identifier: Apache-2.0
#include "GVDBPointStreamConversion.h"
#include <algorithm>
#include <cmath>

namespace gvdb::portable {
namespace {
bool finite(Float4 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}
uint64_t endByte(const PointStream& stream) {
    return stream.byteOffset + (stream.count - 1) * stream.byteStride +
           stream.components * uint64_t(componentBytes(stream.component));
}
}  // namespace
nvrhi::FRESULT preparePointStreamConversion(const PointStream& source, const PointStream& destination,
                                            const PointStreamTransform& transform,
                                            PointStreamConversion& output) noexcept {
    const auto sourceValid = validatePointStream(source);
    if (!sourceValid)
        return sourceValid.result;
    const auto destinationValid = validatePointStream(destination);
    if (!destinationValid)
        return destinationValid.result;
    if (source.count != destination.count || source.components != destination.components ||
        !finite(transform.minimum) || !finite(transform.delta) || !finite(transform.scale) ||
        !finite(transform.translation))
        return nvrhi::FE_INVALID_ARGS;
    PointStreamConversion candidate;
    if (!source.count) {
        output = std::move(candidate);
        return nvrhi::FS_OK;
    }
    if (source.buffer == destination.buffer)
        return nvrhi::FE_UNSUPPORTED;
    if (!destination.buffer->getDesc().canHaveUAVs)
        return nvrhi::FE_INVALID_ARGS;
    const uint64_t sourceEnd = endByte(source), destinationEnd = endByte(destination);
    if (((sourceEnd + 3) & ~uint64_t(3)) > source.buffer->getDesc().byteSize ||
        ((destinationEnd + 3) & ~uint64_t(3)) > destination.buffer->getDesc().byteSize)
        return nvrhi::FE_INVALID_ARGS;
    const uint32_t firstWord = uint32_t(destination.byteOffset / 4);
    const uint32_t wordCount = uint32_t((destinationEnd + 3) / 4 - firstWord);
    const uint32_t groups = wordCount / 256 + uint32_t(wordCount % 256 != 0);
    candidate.source = source.buffer;
    candidate.destination = destination.buffer;
    candidate.parameters = {
        {uint32_t(source.count), componentBytes(source.component),      uint32_t(source.byteOffset),
         uint32_t(source.byteStride)                                                                       },
        {source.components,      componentBytes(destination.component), uint32_t(destination.byteOffset),
         uint32_t(destination.byteStride)                                                                  },
        {firstWord,              wordCount,                             0,                                0},
        transform.minimum,
        transform.delta,
        transform.scale,
        transform.translation
    };
    candidate.groupsX = std::min(groups, 65535u);
    candidate.groupsY = groups / 65535u + uint32_t(groups % 65535u != 0);
    output = std::move(candidate);
    return nvrhi::FS_OK;
}
}  // namespace gvdb::portable

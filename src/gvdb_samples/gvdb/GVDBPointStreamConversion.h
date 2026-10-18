// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhi.h"

namespace gvdb::portable {
// The source and destination keep their point/component order. Conversion uses
// binary32 arithmetic: (minimum + value * delta) * scale + translation.
// Exact identity transforms bypass arithmetic, preserving signed zero/subnormals.
// Float64 input rounds to binary32 first; Float64 output exactly widens that
// binary32 result. UInt8/UInt16 output clamps and truncates; nonfinite maps to 0.
// Non-identity arithmetic follows the target's binary32 denormal handling.
struct PointStreamTransform {
    Float4 minimum{};
    Float4 delta{1, 1, 1, 1};
    Float4 scale{1, 1, 1, 1};
    Float4 translation{};
};
struct alignas(16) PointStreamConversionParameters {
    slang::UInt4 source;       // count, component bytes, byte offset, byte stride
    slang::UInt4 destination;  // component count, component bytes, byte offset, byte stride
    slang::UInt4 work;         // first destination word, word count, reserved, reserved
    Float4 minimum, delta, scale, translation;
};
static_assert(sizeof(PointStreamConversionParameters) == 112);
struct PointStreamConversion {
    nvrhi::BufferHandle source, destination;
    PointStreamConversionParameters parameters{};
    uint32_t groupsX = 0, groupsY = 0;
};
// CPU-only validation/packing for the convertPointStream entry. Arbitrary byte
// offsets and strides are supported. Both raw allocations require 4-byte tail
// padding through the final accessed word; the destination must allow UAVs.
// Gaps, prefix, suffix and other interleaved attributes are preserved. A dispatch
// owns each destination 32-bit word exactly once. Source/destination must be
// distinct, non-overlapping allocations: same interface identity is rejected;
// native aliasing across imported handles remains the caller's responsibility.
// This helper does not authenticate external-buffer device provenance or record
// commands. The future operation factory will own that recording contract.
// On failure output is unchanged. Zero elements produce a no-dispatch result.
// Retained legacy conversion only supported ushort3/float3 -> tight float3;
// other encodings, strides and component counts here are explicit extensions.
nvrhi::FRESULT preparePointStreamConversion(const PointStream& source, const PointStream& destination,
                                            const PointStreamTransform&,
                                            PointStreamConversion& output) noexcept;
}  // namespace gvdb::portable

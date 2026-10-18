// Packed point conversion without native shader doubles or byte write races.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "slang/GVDBPortable.h"
#include <nvrhi/nvrhi.h>
namespace ethereal::slang {class ComputeRuntime;}
namespace gvdb::slang {
// Input is a raw buffer with 4-byte trailing padding. Byte/ushort components are
// unsigned integers; 4/8-byte components are IEEE float/double, little-endian.
// Output float4 is (minimum+input*delta)*scale+translation, with w=1.
nvrhi::BufferHandle convertPackedPoints(ethereal::slang::ComputeRuntime& runtime,nvrhi::IBuffer* source,
    PointConversionParameters parameters);
// Output is a tightly packed ushort3 stream (6-byte stride), padded to 4 bytes.
// Components are truncated after normalization; out-of-range values clamp and
// nonfinite components encode as zero. Decode with delta=range/65535.
nvrhi::BufferHandle quantizePointsU16(ethereal::slang::ComputeRuntime& runtime,nvrhi::IBuffer* positions,
    uint32_t count,Float4 minimum,Float4 range);
} // namespace gvdb::slang

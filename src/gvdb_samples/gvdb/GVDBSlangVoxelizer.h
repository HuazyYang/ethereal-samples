// NVRHI solid mesh voxelization. No CUDA texture or kernel-pointer interfaces.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "slang/GVDBPortable.h"
#include <nvrhi/nvrhi.h>
#include <vector>
namespace ethereal::slang {class ComputeRuntime;}
namespace gvdb::slang {
struct VoxelizedGrid {
    nvrhi::BufferHandle values;
    UInt4 resolution;
    VoxelFormat format;
    size_t byteSize;
};
// vertices use xyz; indices are a flat triangle list. Parameters contain the
// model-to-grid transform, finite grid bounds, resolution, values and format.
// counts/values.w/resolution.w are derived by this function. The output is dense
// and tightly packed; R8 buffers have up to three trailing padding bytes.
VoxelizedGrid voxelizeMesh(ethereal::slang::ComputeRuntime& runtime,const std::vector<Float4>& vertices,
    const std::vector<uint32_t>& indices,VoxelizeParameters parameters,VoxelFormat format=VoxelFormat::R32_FLOAT);
} // namespace gvdb::slang

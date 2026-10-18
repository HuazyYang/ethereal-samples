// GVDB scalar-volume operations using the unified NVRHI Slang runtime.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "slang/GVDBPortableData.h"
#include <nvrhi/nvrhi.h>
#include <vector>
#include <array>
namespace ethereal::slang { class ComputeRuntime; }
namespace gvdb::slang {
struct RaycastOutput {
    nvrhi::BufferHandle color;
    nvrhi::BufferHandle depth;
    uint32_t width = 0, height = 0;
};
// Owns NVRHI resources for one packed hierarchy and one R32_FLOAT atlas.
// The borrowed ComputeRuntime must outlive this object. Construction requires a
// successful packVolume result; it performs no legacy CUDA initialization.
class SlangVolume {
    ethereal::slang::ComputeRuntime* m_runtime;
    VolumeInfo m_volume;
    nvrhi::BufferHandle m_nodes, m_children, m_levels, m_atlasMap;
    nvrhi::BufferHandle m_atlas, m_scratch;
    size_t m_voxelCount;
    void operate(const char* entry, OperatorParameters parameters, nvrhi::IBuffer* source=nullptr);
public:
    SlangVolume(ethereal::slang::ComputeRuntime& runtime, const PackedVolume& volume,
        const float* atlas, size_t voxelCount);
    const VolumeInfo& info() const { return m_volume; }
    nvrhi::IBuffer* atlas() const { return m_atlas; }
    nvrhi::IBuffer* nodes() const { return m_nodes; }
    nvrhi::IBuffer* children() const { return m_children; }
    nvrhi::IBuffer* levels() const { return m_levels; }
    void clear();
    void fill(float value);
    void updateApron(float boundary = 0);
    void smooth(float centerWeight, float bias, uint32_t iterations, float boundary = 0);
    void grow(float amount, uint32_t iterations = 1);
    void noise(float amplitude);
    void cut();
    void resample(nvrhi::IBuffer* source,UInt4 sourceResolution,const std::array<Float4,3>& transform,
        Float4 remap,float background=0);
    float interiorSum();
    RaycastOutput raycast(RaycastParameters parameters, const std::vector<Float4>& transfer, bool surface = false);
    std::vector<float> readAtlas();
};
// Integrates axis-aligned source boxes; transform maps destination voxel bounds
// directly to source coordinates. The source must be StructuredBuffer<float>.
std::vector<float> downsampleDense(ethereal::slang::ComputeRuntime& runtime,nvrhi::IBuffer* source,
    UInt4 sourceResolution,UInt4 destinationResolution,const std::array<Float4,3>& transform,Float4 remap);
} // namespace gvdb::slang

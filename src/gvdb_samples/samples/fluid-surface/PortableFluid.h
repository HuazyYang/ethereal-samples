// FLUIDS-v3 SPH parameters and sparse level-set reconstruction without native SDKs.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gvdb/GVDBSlangSph.h>
#include <gvdb/GVDBNvrhiApi.h>
#include <gvdb/slang/GVDBPortableData.h>

#include <memory>
namespace gvdb::samples::fluid {
struct InitialFluid {
    slang::SphParameters parameters{};
    std::vector<slang::Float4> positions;
    std::vector<uint32_t> colors;
};
// Full legacy wave-pool preset. compact selects an explicit small regression scene.
InitialFluid makeInitialFluid(uint32_t requested = 1500000, bool compact = false, uint32_t seed = 6572);
// Leaf-first {5,3,3,3,3}: the retained fromBranching(3,3,3,3,5) preset.
// Full-rebuild support is floor(position)-2 through floor(position)+4, inclusive.
slang::PackedVolume buildFluidTopology(const std::vector<slang::Float4>& positions);
class FluidSurface {
    ethereal::slang::ComputeRuntime* m_runtime;
    struct State;
    std::unique_ptr<State> m_state;
    std::vector<slang::Float4> m_positions;

 public:
    explicit FluidSurface(ethereal::slang::ComputeRuntime& runtime);
    ~FluidSurface();
    // Radius-one level set, support radius two, four-voxel subcells, background three.
    void reconstruct(nvrhi::IBuffer* positions, uint32_t count, nvrhi::IBuffer* colors = nullptr);
    portable::SnapshotHandle snapshot() const;
    const portable::IGVDBHostVolume& topology() const;
    // Explicit diagnostics; reconstruction and rendering use the public publication.
    slang::PackedVolume readTopology() const;
    std::vector<float> readAtlas() const;
    std::vector<uint32_t> readColor() const;
    const std::vector<slang::Float4>& positions() const { return m_positions; }
};
}  // namespace gvdb::samples::fluid

// Deterministic original wand motion and fixed-topology material deposition.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gvdb/GVDBNvrhiApi.h>
#include <gvdb/GVDBSlangRayQuery.h>
#include <sample-utils/SlangSceneRT.h>
#include <memory>
#include <random>
namespace gvdb::samples::spray {
enum class WandStyle : uint32_t {
    Rotate,
    Sweep,
    Wave
};
struct SprayRays {
    std::vector<slang::Float4> origins, directions, hits, normals;
    std::vector<uint32_t> colors;
};
class SprayWand {
    std::mt19937 m_rng{6572};
    uint32_t m_time = 0;

 public:
    SprayRays next(WandStyle style, uint32_t count = 1000);
    uint32_t time() const { return m_time; }
};
class SprayVolume {
    ethereal::slang::ComputeRuntime* m_runtime;
    struct State;
    std::unique_ptr<State> m_state;
    SprayWand m_wand;
    SprayRays m_rays;
    uint32_t m_hitCount = 0;

 public:
    SprayVolume(ethereal::slang::ComputeRuntime&, slang::PackedVolume, const std::vector<float>&,
                uint32_t initialColor = 0xffb2b2b2u);
    SprayVolume(ethereal::slang::ComputeRuntime&, portable::VolumeHandle);
    ~SprayVolume();
    void step(WandStyle);
    void smooth(uint32_t iterations);
    void refreshAprons(uint32_t mask);
    // Accumulates into existing topology only. Caller supplies float4 index positions
    // and packed RGBA8 colors; nonfinite samples are skipped before canonical conversion.
    void depositPoints(nvrhi::IBuffer* positions, nvrhi::IBuffer* colors, uint32_t count);
    portable::SnapshotHandle snapshot() const;
    const portable::IGVDBHostVolume& topology() const;
    std::vector<float> readAtlas() const;
    std::vector<uint8_t> readColor() const;
    slang::PackedVolume readTopology() const;
    double interiorSum() const;
    const SprayRays& rays() const { return m_rays; }
    uint32_t time() const { return m_wand.time(); }
    uint32_t hitCount() const { return m_hitCount; }
};
std::unique_ptr<SprayVolume> voxelizeSprayPart(ethereal::slang::ComputeRuntime&, const sample::SceneMesh&,
                                               float scale = 200,
                                               slang::Float4 translation = {200, 200, 200, 0});
}  // namespace gvdb::samples::spray

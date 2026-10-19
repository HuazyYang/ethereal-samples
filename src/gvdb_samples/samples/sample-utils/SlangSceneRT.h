// Mixed mesh/volume renderer using NVRHI ray tracing and offline Slang.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SceneMaterial.h"
#include <gvdb/GVDBSlangVolume.h>
#include <gvdb/GVDBNvrhiApi.h>
#include <filesystem>
#include <memory>
#include <vector>

namespace gvdb::sample {
enum class SceneVolumeMode : uint32_t {
    Surface,
    Deep,
    LevelSet,
    EmptySkip
};
struct SceneMesh {
    std::vector<slang::Float4> positions, normals;
    std::vector<uint32_t> indices;
};
struct SceneVolume {
    slang::VolumeInfo info{};
    nvrhi::BufferHandle scalar, nodes, children, levels, color;
    // Raw snapshot channels are consumed directly, without a staging atlas copy.
    portable::SnapshotHandle ownedSnapshot;
    bool rawScalar = false;
    // Raw view bases are 16-byte aligned; level bases align to sizeof(LevelInfo).
    nvrhi::BufferRange scalarRange, nodesRange, childrenRange, levelsRange, colorRange;
    std::vector<slang::Float4> transfer;
    // Inclusive lookup endpoint; UINT32_MAX uses the full table. Legacy 16384-entry
    // tables use 16300 explicitly, leaving their original reserved tail intact.
    uint32_t transferLastIndex = UINT32_MAX;
    SceneVolumeMode mode = SceneVolumeMode::Deep;
    // Legacy GVDB controls: direct/shadow/fine step, brick-entry epsilon;
    // signed Beer coefficient/albedo; scalar cutoff/transmittance cutoff;
    // isovalue/transfer minimum/transfer maximum/missing scalar value.
    slang::Float4 steps{.25f, 16, .25f, .001f};
    slang::Float4 extinct{-1, 1.1f, 0, 0};
    slang::Float4 cutoff{.005f, .001f, 0, 0};
    slang::Float4 threshold{.1f, 0, 1, 0};
    // Snapshot retains GPU buffers. Replace it after a topology/atlas relocation.
    static SceneVolume from(const slang::SlangVolume& volume);
    static SceneVolume from(portable::SnapshotHandle volume, uint32_t scalarChannel = 0,
                            uint32_t colorChannel = UINT32_MAX);
};
struct SceneInstance {
    enum class Kind : uint32_t {
        Mesh,
        Volume
    };
    Kind kind = Kind::Mesh;
    uint32_t geometry = 0, material = 0;
    nvrhi::rt::AffineTransform transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
};
struct SceneDescription {
    std::vector<SceneMesh> meshes;
    std::vector<SceneVolume> volumes;
    std::vector<SceneInstance> instances;
    std::vector<SceneMaterial> materials;
};
struct SceneEnvironment {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
};
class SlangSceneRT {
    struct Impl;
    std::unique_ptr<Impl> m_impl;

 public:
    static constexpr uint32_t MaxVolumes = 8;
    SlangSceneRT(ethereal::slang::ComputeRuntime& runtime, const std::filesystem::path& artifact);
    ~SlangSceneRT();
    void setScene(const SceneDescription& scene);
    void updateVolume(uint32_t index, const SceneVolume& volume);
    void updateMesh(uint32_t index, const SceneMesh& mesh);
    void updateTransform(uint32_t instance, const nvrhi::rt::AffineTransform& transform);
    void setMaterial(uint32_t index, const SceneMaterial& material);
    void setEnvironment(const SceneEnvironment& environment);
    void setPointLight(slang::Float4 worldPosition);
    void setRandomSeed(uint32_t seed, uint32_t dataFrame = 0);
    void resetAccumulation();
    // Camera vectors/clip planes are world space; per-volume marching is index space.
    // Scene/camera/resolution changes reset progressive accumulation automatically.
    // UINT32_MAX advances automatically; zero starts a new sequence.
    // An explicit nonzero sample must equal sampleCount().
    slang::RaycastOutput render(slang::RaycastParameters camera, uint32_t sample = UINT32_MAX);
    uint32_t sampleCount() const;
};
}  // namespace gvdb::sample

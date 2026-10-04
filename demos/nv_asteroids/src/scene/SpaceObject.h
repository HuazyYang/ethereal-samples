#pragma once

// A loadable model (player ship, monolith, ...): a renamed copy of the 2018 donut Scene with
// meshlet buffers on top. Models come from FBX (assimp C API, meshletized at runtime) or from a
// 2018 .chk meshlet file.
//
// Asteroids.exe: SpaceObject (vtable 0x14025AAB0, 0x270 bytes; ctor 0x140050520, dtor 0x140050EB0,
// Load = vfunc04 0x140054DE0, assimp mesh conversion = vfunc03 0x140054760).

#include "scene/MeshletBuilder.h"
#include "scene/SceneGraph.h"
#include "scene/SceneMaterial.h"

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct aiScene;
struct aiNode;
struct SceneLoadProgress;

namespace donut::vfs { class IFileSystem; }
namespace donut::engine { class CommonRenderPasses; class TextureCache; class ThreadPool; }
namespace chunk2018 { class MeshSet; }

// Load flags (SpaceScene passes 63). Bits 0..5 select assimp post-processing, bit 6 forces
// specularTextureType = 3 for the object's materials.
enum SpaceObjectLoadFlags : uint32_t
{
    LoadFlag_JoinIdenticalVertices = 0x01,
    LoadFlag_Unused02 = 0x02,
    LoadFlag_Triangulate = 0x04,
    LoadFlag_CalcTangentSpace = 0x08,
    LoadFlag_GenNormals = 0x10,
    LoadFlag_FlipUVs = 0x20,
    LoadFlag_ForceSpecularType3 = 0x40,
    LoadFlag_Default = 0x3F,
};

// Meshlet buffer ids (2018 0x140054510). Same register assignment as AsteroidBufferType plus t9.
enum class SpaceObjectBufferType : uint32_t
{
    VertexPositions = 0,
    VertexNormals = 1,
    VertexTexcoord1 = 2,
    VertexTexcoord2 = 3,
    VertexTangents = 4,
    VertexBitangents = 5,
    VertexIndices = 6,      // "Vertex Positions Indices (32)"
    TriangleIndices = 7,    // "Triangle Indices (8)"
    MeshletInfo = 8,
    SubElementsInfo = 9,    // uint2 {firstMeshlet, numMeshlets} per mesh info / instance
    LodInfo = 10,
};

struct SpaceObjectLoadContext
{
    donut::engine::TextureCache* textureCache = nullptr;
    donut::engine::ThreadPool* threadPool = nullptr;
    SceneLoadProgress* progress = nullptr;
};

class SpaceObject
{
public:
    explicit SpaceObject(nvrhi::AutoPtr<donut::vfs::IFileSystem> fs);
    ~SpaceObject();
    SpaceObject(const SpaceObject&) = delete;
    SpaceObject& operator=(const SpaceObject&) = delete;

    // vfunc00..02
    const std::vector<SceneMesh*>& GetMeshes() const { return m_Meshes; }
    const std::vector<SceneMeshInstance*>& GetMeshInstances() const { return m_MeshInstances; }
    const std::vector<SceneMaterial*>& GetMaterials() const { return m_MaterialsByIndex; }

    // vfunc04. '.fbx' files go through assimp, anything else is read as a .chk meshlet set.
    // 'materialsFile' (optional) is a materials.json; texture names in it are relative to the model's directory.
    // 'instanceAtMeshlessNodes': assimp only — instance every mesh at nodes without meshes (unused by the demo).
    bool Load(const std::filesystem::path& file, const std::filesystem::path& materialsFile,
        const SpaceObjectLoadContext& context, bool instanceAtMeshlessNodes = false, uint32_t loadFlags = LoadFlag_Default);

    // Per-instance placements from the fscene "instances" array; assimp meshes are instanced once per entry.
    void AddInstanceTransform(const donut::math::affine3& transform) { m_InstanceTransforms.push_back(transform); }
    const std::vector<donut::math::affine3>& GetInstanceTransforms() const { return m_InstanceTransforms; }

    // 0x140052AD0. 'meshlets': build/upload meshlet buffers (else index/vertex + "Transforms" buffers).
    // Material textures must be finalized by the TextureCache before this is called.
    void CreateRenderResources(nvrhi::IDevice* device, nvrhi::ICommandList* commandList,
        donut::engine::CommonRenderPasses& commonPasses, nvrhi::IBindingLayout* materialBindingLayout,
        bool meshlets, bool skipGeometry = false);

    // 0x140051CC0: instance world bounds/centres and the object bounds.
    void UpdateBounds();
    // 0x140058530: re-upload the constants of materials marked dirty (ship glows change opacity).
    void UpdateMaterials(nvrhi::ICommandList* commandList);
    // 0x1400585D0: (re)write the "Transforms" buffer (current + previous float3x4 per instance).
    void UpdateTransformsBuffer(nvrhi::ICommandList* commandList);

    nvrhi::IBuffer* GetMeshletBuffer(SpaceObjectBufferType type) const;
    nvrhi::IBuffer* GetTransformsBuffer() const { return m_TransformsBuffer; }
    const SceneBufferGroup& GetBuffers() const { return m_Buffers; }
    nvrhi::BindingSetHandle& GetMeshletBindingSet() { return m_MeshletBindingSet; }

    const donut::math::box3& GetBounds() const { return m_SceneBounds; }
    const std::shared_ptr<SceneNode>& GetRootNode() const { return m_RootNode; }
    uint32_t GetNumMeshlets() const { return m_NumMeshlets; }
    uint32_t GetNumSubElementMeshlets() const { return m_NumSubElementMeshlets; }
    const donut::math::box3& GetMeshletBounds() const { return m_MeshletBounds; }   // quantization bounds of the meshlet culling data
    const SceneMeshletData* GetMeshletData() const { return m_MeshletData.get(); }

    // Copied into the meshlet per-object constants (2018 +560): x = engine glow intensity,
    // y / z = nav-light blink states. All 1 by default; driven by CargoShip.
    donut::math::float4 shipGlowParams = donut::math::float4(1.f);
    // Set by SpaceScene on the player ship's object (2018 +576).
    bool isPlayerShip = false;

private:
    bool LoadAssimp(const std::filesystem::path& file, bool hasMaterialsJson, const SpaceObjectLoadContext& context,
        bool instanceAtMeshlessNodes, uint32_t loadFlags);
    bool LoadChunkFile(const std::filesystem::path& file, bool hasMaterialsJson);
    void LoadMaterialsJson(const std::filesystem::path& materialsFile, const SpaceObjectLoadContext& context);
    void ImportAssimpMaterials(const aiScene* scene, const SpaceObjectLoadContext& context);
    void ResolveMaterials(const aiScene* scene);
    void ResolveMaterials(const chunk2018::MeshSet& meshSet);
    void CreateMeshesFromAssimp(const aiScene* scene);
    void ProcessNode(const aiScene* scene, const aiNode* node, const std::shared_ptr<SceneNode>& sceneNode,
        const donut::math::affine3& parentWorld, uint32_t depth, bool instanceAtMeshlessNodes);

    void CreateGeometryBuffers(nvrhi::IDevice* device, nvrhi::ICommandList* commandList);
    void CreateTransformsBuffer(nvrhi::IDevice* device, nvrhi::ICommandList* commandList);
    void BuildMeshlets(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, bool skipBuffers);
    void CreateMeshletBuffers(nvrhi::IDevice* device, nvrhi::ICommandList* commandList);
    void CreateChunkMeshletBuffers(nvrhi::IDevice* device, nvrhi::ICommandList* commandList);
    void CreateMaterialResources(nvrhi::IDevice* device, nvrhi::ICommandList* commandList,
        donut::engine::CommonRenderPasses& commonPasses, nvrhi::IBindingLayout* materialBindingLayout);
    void SortInstances();
    void ClearCpuData();

    donut::math::box3 m_SceneBounds = donut::math::box3::empty();
    std::shared_ptr<SceneNode> m_RootNode;
    std::vector<SceneMesh*> m_Meshes;                   // owned
    std::vector<SceneMeshInstance*> m_MeshInstances;    // owned
    std::vector<SceneMaterial*> m_MaterialsByIndex;     // non-owning, entries may be null
    MaterialMap m_MaterialsByName;                      // owning (materials.json entries)
    std::vector<std::shared_ptr<SceneMaterial>> m_ImportedMaterials;    // owning (assimp-imported materials)
    SceneBufferGroup m_Buffers;
    uint32_t m_LoadFlags = 0;
    nvrhi::AutoPtr<donut::vfs::IFileSystem> m_FS;
    std::filesystem::path m_Directory;
    std::vector<donut::math::affine3> m_InstanceTransforms;
    uint32_t m_EnabledAttributes = VertexAttr_All;

    std::vector<uint32_t> m_Indices;
    std::vector<donut::math::float3> m_Positions;
    std::vector<donut::math::float2> m_Texcoord1;
    std::vector<donut::math::float2> m_Texcoord2;
    std::vector<uint32_t> m_Normals;        // snorm8 x3
    std::vector<uint32_t> m_Tangents;
    std::vector<uint32_t> m_Bitangents;

    nvrhi::BufferHandle m_TransformsBuffer;             // "Transforms"
    std::unique_ptr<SceneMeshletData> m_MeshletData;
    std::shared_ptr<chunk2018::MeshSet> m_ChunkMeshSet; // non-null when loaded from a .chk file

    nvrhi::BufferHandle m_VertexIndicesBuffer;          // "Vertex Positions Indices (32)"
    nvrhi::BufferHandle m_TriangleIndicesBuffer;        // "Triangle Indices (8)"
    nvrhi::BufferHandle m_MeshletInfoBuffer;            // "Meshlet Info"
    nvrhi::BufferHandle m_SubElementsBuffer;            // "SubElements Info"
    nvrhi::BufferHandle m_LodInfoBuffer;                // "LOD Info"
    nvrhi::BufferHandle m_PositionsBuffer;              // "Vertex Positions"
    nvrhi::BufferHandle m_NormalsBuffer;                // "Vertex Normals"
    nvrhi::BufferHandle m_Texcoord1Buffer;              // "Vertex Texcoord1"
    nvrhi::BufferHandle m_Texcoord2Buffer;              // "Vertex Texcoord2"
    nvrhi::BufferHandle m_TangentsBuffer;               // "Vertex Tangents"
    nvrhi::BufferHandle m_BitangentsBuffer;             // "Vertex Bitangents"

    uint32_t m_NumMeshlets = 0;
    uint32_t m_NumSubElementMeshlets = 0;
    donut::math::box3 m_MeshletBounds = donut::math::box3::empty();
    nvrhi::BindingSetHandle m_MeshletBindingSet;
};

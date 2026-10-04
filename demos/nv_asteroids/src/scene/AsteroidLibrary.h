#pragma once

// Asteroid type library: asteroids_chk-lod.json -> per-type LOD chain of 2018 .chk meshlet sets ->
// concatenated GPU buffers consumed by the asteroid meshlet draw path (asteroidTS / asteroidMS).
//
// Asteroids.exe: AsteroidTypeLibrary (0x38 bytes, owned by SpaceScene+120; load 0x1400101C0),
// AsteroidType (0x298 bytes; ctor 0x14000D9C0, load 0x14000E420, GPU resources 0x140012770),
// AsteroidLod (0x40 bytes; load 0x1400111C0).

#include "scene/SceneMaterial.h"

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Json { class Value; }
namespace donut::vfs { class IFileSystem; }
namespace donut::engine { class CommonRenderPasses; class TextureCache; class ThreadPool; }
namespace chunk2018 { class MeshSet; }
namespace physx { class PxCooking; class PxPhysics; class PxTriangleMesh; }

constexpr uint32_t c_MaxAsteroidLods = 10;
constexpr uint32_t c_InvalidAsteroidTypeId = ~0u;

// Loading progress shown on the "LOADING" screen (2018 global qword_1402E02C0).
struct SceneLoadProgress
{
    std::atomic<int> total{ 0 };
    std::atomic<int> completed{ 0 };
};

// StructuredBuffer<LODInfo> lodInfoBuffer (asteroidTS/MS reflection). Index and meshlet data stay
// LOD-local in the buffers; the shaders add these start offsets.
struct AsteroidLODInfo
{
    uint32_t indexStart;    // into "Vertex Indices" (u32 meshlet vertex lists)
    uint32_t numIndices;
    uint32_t vertexStart;   // into the vertex attribute buffers
    uint32_t numVertices;
    uint32_t primStart;     // byte offset into "Triangle Indices" (u8, 3 per triangle)
    uint32_t numPrims;      // number of u8 primitive indices
    uint32_t minfoStart;    // into "Meshlet Info"
    uint32_t numMinfo;      // number of meshlet headers
};
static_assert(sizeof(AsteroidLODInfo) == 32);

struct AsteroidBoundingBox
{
    donut::math::float4 bboxMin;    // w = 1
    donut::math::float4 bboxMax;    // w = 1
};

// cbuffer cbObjectInfo (b4, space1) — struct ObjectConstants. The HLSL cbuffer reports 404 bytes
// because 'padding[2]' is laid out with 16-byte array elements; the 2018 upload is 384 bytes.
struct AsteroidObjectConstants
{
    AsteroidBoundingBox bbox;
    AsteroidBoundingBox bboxLods[c_MaxAsteroidLods];   // empty boxes (FLT_MAX / -FLT_MAX) past the last LOD
    donut::math::float3 center;
    float radius;
    float lodBias;
    float maxLevelToRender;
    uint32_t padding[2];
};
static_assert(sizeof(AsteroidObjectConstants) == 384);

// Buffer ids of AsteroidType::GetBuffer (2018 0x140010060). The value is also the SRV register
// the meshlet draw strategy binds the buffer to (t0..t8, t10).
enum class AsteroidBufferType : uint32_t
{
    VertexPositions = 0,    // StructuredBuffer<float3>
    VertexNormals = 1,      // StructuredBuffer<uint>  (snorm8 x3)
    VertexTexcoord1 = 2,    // StructuredBuffer<float2>
    VertexTexcoord2 = 3,    // StructuredBuffer<float2>
    VertexTangents = 4,     // StructuredBuffer<uint>
    VertexBitangents = 5,   // StructuredBuffer<uint>
    VertexIndices = 6,      // StructuredBuffer<uint>  (meshlet vertex lists)
    TriangleIndices = 7,    // ByteAddressBuffer (u8 local triangle indices)
    MeshletInfo = 8,        // StructuredBuffer<uint4> (chunk2018::MeshletHeader)
    LodInfo = 10,           // StructuredBuffer<AsteroidLODInfo>
};

// One LOD of an asteroid type (one .chk file).
class AsteroidLod
{
public:
    // Reads and validates the chunk file: must be a meshlet set built for
    // c_MeshletMaxVerts / c_MeshletMaxPrims (64 / 100).
    bool Load(donut::vfs::IFileSystem& fs, const std::filesystem::path& path);

    SceneMaterial* material = nullptr;     // material of the last meshlet info (2018 behaviour)
    donut::math::box3 bounds = donut::math::box3::empty();
    std::shared_ptr<chunk2018::MeshSet> meshSet;   // CPU data; released after the GPU upload
    uint32_t numVertices = 0;
    uint32_t numIndices = 0;
    uint32_t numPrims = 0;      // u8 primitive indices (3 per triangle)
    uint32_t numMeshlets = 0;
};

struct AsteroidLodDesc
{
    int level = 0;
    std::filesystem::path model;
};

struct AsteroidTypeDesc
{
    std::string name;
    int maxLevelToRender = 9;
    std::filesystem::path materialFile;
    float lodBias = 0.f;
    uint32_t typeId = 0;
    int collisionLod = 5;
    std::vector<AsteroidLodDesc> lods;
};

struct AsteroidLoadContext
{
    std::shared_ptr<donut::vfs::IFileSystem> fs;
    donut::engine::TextureCache* textureCache = nullptr;
    donut::engine::ThreadPool* threadPool = nullptr;    // optional: parallel type loading and texture decode
    SceneLoadProgress* progress = nullptr;
    physx::PxCooking* cooking = nullptr;                // optional: collision mesh cooking
    physx::PxPhysics* physics = nullptr;
};

class AsteroidType
{
public:
    explicit AsteroidType(uint32_t numLods);
    ~AsteroidType();

    void Load(const AsteroidTypeDesc& desc, const AsteroidLoadContext& context);

    // Creates the per-type buffers and material resources, uploads every LOD, the LOD table and the
    // object constants, then releases the CPU chunk data. 'commandList' must be open.
    void CreateRenderResources(nvrhi::ICommandList* commandList, donut::engine::CommonRenderPasses& commonPasses,
        nvrhi::IBindingLayout* materialBindingLayout);

    // Re-uploads the object constants (lodBias etc. edited at runtime).
    void UpdateObjectConstants(nvrhi::ICommandList* commandList);

    nvrhi::IBuffer* GetBuffer(AsteroidBufferType type) const;
    nvrhi::IBuffer* GetObjectConstantsBuffer() const { return m_ObjectConstantsBuffer; }

    const std::string& GetName() const { return m_Name; }
    const std::vector<SceneMaterial*>& GetMaterials() const { return m_Materials; }
    SceneMaterial* GetMaterial() const { return m_Materials.empty() ? nullptr : m_Materials[0]; }
    const std::vector<std::unique_ptr<AsteroidLod>>& GetLods() const { return m_Lods; }
    const std::vector<AsteroidLODInfo>& GetLodInfos() const { return m_LodInfos; }
    uint32_t GetNumLods() const { return uint32_t(m_Lods.size()); }
    const donut::math::box3& GetBounds() const { return m_Bounds; }

    AsteroidObjectConstants& GetObjectConstants() { return m_ObjectConstants; }
    const AsteroidObjectConstants& GetObjectConstants() const { return m_ObjectConstants; }
    float GetLodBias() const { return m_ObjectConstants.lodBias; }
    float GetMaxLevelToRender() const { return m_ObjectConstants.maxLevelToRender; }
    const donut::math::float3& GetCenter() const { return m_ObjectConstants.center; }
    float GetRadius() const { return m_ObjectConstants.radius; }

    // u8 primitive-index count of the last LOD (2018 0x140010160; used for triangle statistics).
    uint32_t GetNumPrims() const { return m_LodInfos.empty() ? 0 : m_LodInfos.back().numPrims; }
    uint32_t GetTotalVertices() const { return m_TotalVertices; }
    uint32_t GetTotalIndices() const { return m_TotalIndices; }
    uint32_t GetTotalPrims() const { return m_TotalPrims; }
    uint32_t GetTotalMeshlets() const { return m_TotalMeshlets; }

    physx::PxTriangleMesh* GetCollisionMesh() const { return m_CollisionMesh; }

    // Binding set cache slot owned by the meshlet draw strategy (2018 AsteroidType+648).
    nvrhi::BindingSetHandle& GetMeshletBindingSet() { return m_MeshletBindingSet; }

private:
    void LoadMaterials(donut::vfs::IFileSystem& fs, const std::filesystem::path& materialFile,
        const AsteroidLoadContext& context);
    void ResolveMaterials(const chunk2018::MeshSet& meshSet);
    void CreateCollisionMesh(const AsteroidLod& lod, const AsteroidLoadContext& context);
    void ComputeBoundsAndLodInfos();
    bool CreateBuffers(nvrhi::IDevice* device);

    std::string m_Name;
    std::vector<SceneMaterial*> m_Materials;    // indexed by chunk-file material id (non-owning)
    MaterialMap m_MaterialsByName;              // owning
    std::vector<std::unique_ptr<AsteroidLod>> m_Lods;
    std::vector<AsteroidLODInfo> m_LodInfos;
    donut::math::box3 m_Bounds = donut::math::box3::empty();
    AsteroidObjectConstants m_ObjectConstants{};
    // unresolved: 2018 AsteroidType+536 is set to 0x3D; its bit 0x40 (never set) would force
    // specularTextureType = 3 for the type's materials.
    uint32_t m_AttributeFlags = 0x3D;

    uint32_t m_TotalVertices = 0;
    uint32_t m_TotalIndices = 0;
    uint32_t m_TotalPrims = 0;
    uint32_t m_TotalMeshlets = 0;

    nvrhi::BufferHandle m_VertexIndicesBuffer;      // "Vertex Indices"
    nvrhi::BufferHandle m_TriangleIndicesBuffer;    // "Triangle Indices"
    nvrhi::BufferHandle m_MeshletInfoBuffer;        // "Meshlet Info"
    nvrhi::BufferHandle m_LodInfoBuffer;            // "LOD Info"
    nvrhi::BufferHandle m_PositionsBuffer;          // "Vertex Positions"
    nvrhi::BufferHandle m_NormalsBuffer;            // "Vertex Normals"
    nvrhi::BufferHandle m_Texcoord1Buffer;          // "Vertex Texcoord1"
    nvrhi::BufferHandle m_Texcoord2Buffer;          // "Vertex Texcoord2"
    nvrhi::BufferHandle m_TangentsBuffer;           // "Vertex Tangents"
    nvrhi::BufferHandle m_BitangentsBuffer;         // "Vertex Bitangents"
    nvrhi::BufferHandle m_ObjectConstantsBuffer;    // "Object Constants"
    nvrhi::BindingSetHandle m_MeshletBindingSet;

    physx::PxTriangleMesh* m_CollisionMesh = nullptr;
};

// Asteroids.exe: AsteroidTypeLibrary (load 0x1400101C0, name lookup 0x14000FF60, typeId lookup 0x14000FEF0)
class AsteroidLibrary
{
public:
    // 'root' is the parsed asteroids_chk-lod.json (top-level array). Paths inside are relative to 'mediaPath'.
    bool Load(const Json::Value& root, const std::filesystem::path& mediaPath, const AsteroidLoadContext& context);

    void CreateRenderResources(nvrhi::IDevice* device, donut::engine::CommonRenderPasses& commonPasses,
        nvrhi::IBindingLayout* materialBindingLayout);

    // Uploads the object constants of the types marked dirty (2018 0x1400126A0, called every frame).
    void UpdateObjectConstants(nvrhi::ICommandList* commandList);
    void MarkObjectConstantsDirty(uint32_t typeId);

    uint32_t GetNumTypes() const { return uint32_t(m_Types.size()); }
    // Returns c_InvalidAsteroidTypeId (and logs) if no type has that name.
    uint32_t GetAsteroidTypeId(const std::string& name) const;
    // Returns null (and logs) for an invalid id.
    std::shared_ptr<AsteroidType> GetAsteroidType(uint32_t typeId) const;
    const std::vector<std::shared_ptr<AsteroidType>>& GetTypes() const { return m_Types; }

    // Convenience accessors used by the meshlet draw strategy (2018 SpaceScene 0x14005C260..0x14005C600).
    nvrhi::IBuffer* GetBuffer(uint32_t typeId, AsteroidBufferType type) const;
    nvrhi::IBuffer* GetObjectConstantsBuffer(uint32_t typeId) const;
    SceneMaterial* GetMaterial(uint32_t typeId) const;
    donut::math::box3 GetBounds(uint32_t typeId) const;
    uint32_t GetNumPrims(uint32_t typeId) const;

private:
    std::vector<std::shared_ptr<AsteroidType>> m_Types;     // index = typeId
    std::vector<uint32_t> m_DirtyTypeIds;
};

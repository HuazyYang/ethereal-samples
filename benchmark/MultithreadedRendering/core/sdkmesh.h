#pragma once
// CPU-side reader of the .sdkmesh format (the DirectX SDK sample mesh container): parses the file
// into views of its vertex/index buffers, meshes, subsets, materials and frame tree. No graphics
// API: both renderers build their own buffers and textures from this.
//
// Struct layouts are those of DXUT's SDKMesh.h (Microsoft, MIT license); SquidRoom.sdkmesh is
// version 101. Only what the multithreaded-rendering sample needs is exposed: no animation, no
// adjacency index buffers.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace MtScene
{

#define SDKMESH_FILE_VERSION 101
#define MAX_VERTEX_ELEMENTS 32
#define MAX_VERTEX_STREAMS 16
#define MAX_FRAME_NAME 100
#define MAX_MESH_NAME 100
#define MAX_SUBSET_NAME 100
#define MAX_MATERIAL_NAME 100
#define MAX_TEXTURE_NAME 260
#define MAX_MATERIAL_PATH 260
#define SDKMESH_INVALID_FRAME 0xffffffffu
#define SDKMESH_INVALID_MESH 0xffffffffu

enum SdkMeshPrimitiveType : uint32_t
{
    SDKMESH_PT_TRIANGLE_LIST = 0,
    SDKMESH_PT_TRIANGLE_STRIP,
    SDKMESH_PT_LINE_LIST,
    SDKMESH_PT_LINE_STRIP,
    SDKMESH_PT_POINT_LIST,
    SDKMESH_PT_TRIANGLE_LIST_ADJ,
    SDKMESH_PT_TRIANGLE_STRIP_ADJ,
    SDKMESH_PT_LINE_LIST_ADJ,
    SDKMESH_PT_LINE_STRIP_ADJ,
};

enum SdkMeshIndexType : uint32_t
{
    SDKMESH_IT_16BIT = 0,
    SDKMESH_IT_32BIT,
};

// D3DVERTEXELEMENT9 (8 bytes)
struct SdkMeshVertexElement
{
    uint16_t Stream;
    uint16_t Offset;
    uint8_t  Type;
    uint8_t  Method;
    uint8_t  Usage;
    uint8_t  UsageIndex;
};

// Types and usages used by SquidRoom (D3DDECLTYPE_* / D3DDECLUSAGE_*)
enum SdkMeshDeclType : uint8_t
{
    SDKMESH_DECLTYPE_FLOAT1 = 0,
    SDKMESH_DECLTYPE_FLOAT2 = 1,
    SDKMESH_DECLTYPE_FLOAT3 = 2,
    SDKMESH_DECLTYPE_FLOAT4 = 3,
    SDKMESH_DECLTYPE_DEC3N = 14,     // 10:10:10 signed normalized, aliased as R10G10B10A2_UNORM by the shaders
    SDKMESH_DECLTYPE_FLOAT16_2 = 15,
    SDKMESH_DECLTYPE_FLOAT16_4 = 16,
    SDKMESH_DECLTYPE_UNUSED = 17,
};

enum SdkMeshDeclUsage : uint8_t
{
    SDKMESH_DECLUSAGE_POSITION = 0,
    SDKMESH_DECLUSAGE_NORMAL = 3,
    SDKMESH_DECLUSAGE_TEXCOORD = 5,
    SDKMESH_DECLUSAGE_TANGENT = 6,
};

#pragma pack(push, 8)
struct SdkMeshHeader
{
    uint32_t Version;
    uint8_t  IsBigEndian;
    uint64_t HeaderSize;
    uint64_t NonBufferDataSize;
    uint64_t BufferDataSize;

    uint32_t NumVertexBuffers;
    uint32_t NumIndexBuffers;
    uint32_t NumMeshes;
    uint32_t NumTotalSubsets;
    uint32_t NumFrames;
    uint32_t NumMaterials;

    uint64_t VertexStreamHeadersOffset;
    uint64_t IndexStreamHeadersOffset;
    uint64_t MeshDataOffset;
    uint64_t SubsetDataOffset;
    uint64_t FrameDataOffset;
    uint64_t MaterialDataOffset;
};

struct SdkMeshVertexBufferHeader
{
    uint64_t NumVertices;
    uint64_t SizeBytes;
    uint64_t StrideBytes;
    SdkMeshVertexElement Decl[MAX_VERTEX_ELEMENTS];
    uint64_t DataOffset; // from the start of the buffer data section... see Load()
};

struct SdkMeshIndexBufferHeader
{
    uint64_t NumIndices;
    uint64_t SizeBytes;
    uint32_t IndexType;
    uint64_t DataOffset;
};

struct SdkMeshMesh
{
    char     Name[MAX_MESH_NAME];
    uint8_t  NumVertexBuffers;
    uint32_t VertexBuffers[MAX_VERTEX_STREAMS];
    uint32_t IndexBuffer;
    uint32_t NumSubsets;
    uint32_t NumFrameInfluences;
    float    BoundingBoxCenter[3];
    float    BoundingBoxExtents[3];
    uint64_t SubsetOffset;
    uint64_t FrameInfluenceOffset;
};

struct SdkMeshSubset
{
    char     Name[MAX_SUBSET_NAME];
    uint32_t MaterialID;
    uint32_t PrimitiveType;
    uint64_t IndexStart;
    uint64_t IndexCount;
    uint64_t VertexStart;
    uint64_t VertexCount;
};

struct SdkMeshFrame
{
    char     Name[MAX_FRAME_NAME];
    uint32_t Mesh;
    uint32_t ParentFrame;
    uint32_t ChildFrame;
    uint32_t SiblingFrame;
    float    Matrix[16];
    uint32_t AnimationDataIndex;
};

struct SdkMeshMaterial
{
    char  Name[MAX_MATERIAL_NAME];
    char  MaterialInstancePath[MAX_MATERIAL_PATH];
    char  DiffuseTexture[MAX_TEXTURE_NAME];
    char  NormalTexture[MAX_TEXTURE_NAME];
    char  SpecularTexture[MAX_TEXTURE_NAME];
    float Diffuse[4];
    float Ambient[4];
    float Specular[4];
    float Emissive[4];
    float Power;
    uint64_t Force64[6]; // the runtime texture/heap slots of DXUT; unused here
};
#pragma pack(pop)

static_assert(sizeof(SdkMeshHeader) == 104, "SDKMESH_HEADER layout");
static_assert(sizeof(SdkMeshVertexBufferHeader) == 24 + 8 * MAX_VERTEX_ELEMENTS + 8, "SDKMESH_VERTEX_BUFFER_HEADER layout");
static_assert(sizeof(SdkMeshIndexBufferHeader) == 32, "SDKMESH_INDEX_BUFFER_HEADER layout");
static_assert(sizeof(SdkMeshMesh) == 224, "SDKMESH_MESH layout");
static_assert(sizeof(SdkMeshSubset) == 144, "SDKMESH_SUBSET layout");
static_assert(sizeof(SdkMeshFrame) == 184, "SDKMESH_FRAME layout");
static_assert(sizeof(SdkMeshMaterial) == 1256, "SDKMESH_MATERIAL layout");

class SdkMesh
{
public:
    // Reads the whole file. On failure returns false and sets `error`.
    bool Load(const std::filesystem::path& path, std::string& error);

    const SdkMeshHeader& Header() const { return *m_Header; }
    uint32_t NumVertexBuffers() const { return m_Header->NumVertexBuffers; }
    uint32_t NumIndexBuffers() const { return m_Header->NumIndexBuffers; }
    uint32_t NumMeshes() const { return m_Header->NumMeshes; }
    uint32_t NumMaterials() const { return m_Header->NumMaterials; }
    uint32_t NumFrames() const { return m_Header->NumFrames; }

    const SdkMeshVertexBufferHeader& VertexBuffer(uint32_t i) const { return m_VertexBuffers[i]; }
    const SdkMeshIndexBufferHeader& IndexBuffer(uint32_t i) const { return m_IndexBuffers[i]; }
    const SdkMeshMesh& Mesh(uint32_t i) const { return m_Meshes[i]; }
    const SdkMeshMaterial& Material(uint32_t i) const { return m_Materials[i]; }
    const SdkMeshFrame& Frame(uint32_t i) const { return m_Frames[i]; }
    const SdkMeshSubset& Subset(const SdkMeshMesh& mesh, uint32_t subsetInMesh) const;

    const uint8_t* VertexData(uint32_t vb) const { return m_VertexData[vb]; }
    const uint8_t* IndexData(uint32_t ib) const { return m_IndexData[ib]; }

    // Directory of the mesh file: material texture names are relative to it.
    const std::filesystem::path& Directory() const { return m_Directory; }

    // Meshes in the order DXUT's CDXUTSDKMesh::Render draws them (depth-first frame tree: node,
    // children, siblings). Both renderers must issue their draws in exactly this order.
    std::vector<uint32_t> RenderOrder() const;

    // Number of (mesh, subset) draws of RenderOrder() and the sum of their index counts.
    void CountDraws(uint64_t& draws, uint64_t& indices) const;

    // FNV-1a of the vertex/index bytes and the material texture names: proves that both renderers
    // rendered the same geometry.
    uint64_t ContentHash() const;

private:
    void VisitFrame(uint32_t frame, std::vector<uint32_t>& order) const;

    std::vector<uint8_t> m_Data;
    std::filesystem::path m_Directory;
    const SdkMeshHeader* m_Header = nullptr;
    const SdkMeshVertexBufferHeader* m_VertexBuffers = nullptr;
    const SdkMeshIndexBufferHeader* m_IndexBuffers = nullptr;
    const SdkMeshMesh* m_Meshes = nullptr;
    const SdkMeshSubset* m_Subsets = nullptr;
    const SdkMeshFrame* m_Frames = nullptr;
    const SdkMeshMaterial* m_Materials = nullptr;
    std::vector<const uint8_t*> m_VertexData;
    std::vector<const uint8_t*> m_IndexData;
};

} // namespace MtScene

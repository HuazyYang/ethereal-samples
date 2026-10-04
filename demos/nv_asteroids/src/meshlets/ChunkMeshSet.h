#pragma once

// 2018 revision of the donut chunk mesh format (.chk), as read by Asteroids.exe
// (ChunkReader 0x1400B28E0 .. 0x1400B3B50, consumer 0x1400111C0).
// The container is identical to donut::chunk::ChunkFile and is parsed with it;
// the chunk descriptors differ from donut main (no MeshInfo::materialName,
// no MeshNode::ctm, an extra materials chunk referenced from the MeshSet).
// See docs/formats.md.

#include <donut/core/chunk/chunkFile.h>
#include <donut/core/math/math.h>
#include <nvrhi/core/autoptr.h>
#include <nvrhi/core/datablob.h>

#include <memory>
#include <string>
#include <vector>

namespace donut::vfs
{
    class nvrhi::IDataBlob;
    class IFileSystem;
}

namespace chunk2018
{
    using donut::chunk::ChunkId;

    enum ChunkType : uint32_t
    {
        CHUNKTYPE_STREAM         = 0x100,
        CHUNKTYPE_STRINGS_TABLE  = 0x110,
        CHUNKTYPE_MESHSET        = 0x200,
        CHUNKTYPE_MESH_INFOS     = 0x201,
        CHUNKTYPE_MESH_INSTANCES = 0x202,
        CHUNKTYPE_MESH_NODES     = 0x203,
        CHUNKTYPE_MATERIALS      = 0x400,
    };
    constexpr uint32_t c_ChunkVersion = 0x100;

    enum StreamType : uint8_t { UINT8 = 1, UINT16, UINT32, FP16, FP32, STRING };
    enum StreamVary : uint8_t { VARY_NONE = 1, VERTEX, FACE, FACE_VERTEX };
    enum StreamSemantic : uint8_t { POSITION = 1, NORMAL, TANGENT, BITANGENT, TEXCOORD, COLOR, INDEX, MESHLET_INFO, USER };

    enum class MeshSetType : uint32_t { Mesh = 0, Meshlet = 1 };

#pragma pack(push, 4)
    struct MeshSetDesc
    {
        enum Streams : uint8_t
        {
            POSITIONS = 0, TEXCOORDS0 = 1, TEXCOORDS1 = 2, NORMALS = 3, TANGENTS = 4, BITANGENTS = 5,
            MESH_INDICES = 6, MESHLET_INDICES32 = 6, MESHLET_INDICES8 = 7, MESHLET_INFO = 8
        };
        uint32_t flags;             // & 0xF : MeshSetType
        uint32_t meshletMaxVerts;
        uint32_t meshletMaxPrims;
        uint32_t unused0;
        uint64_t name;              // string index
        ChunkId streamChunkIds[16];
        ChunkId minfosChunkId;
        ChunkId instancesChunkId;
        ChunkId nodesChunkId;
        ChunkId materialsChunkId;   // 2018 only
        donut::math::box3 bbox;
    };
    static_assert(sizeof(MeshSetDesc) == 128);

    struct MeshInfoBase
    {
        uint64_t name;              // string index
        uint32_t materialId;
        donut::math::box3 bbox;
        uint32_t padding;
    };
    struct MeshletInfo : MeshInfoBase { uint32_t firstMeshlet, numMeshlets; };
    struct MeshInfo : MeshInfoBase { uint32_t firstVertex, numVertices, firstIndex, numIndices; };
    static_assert(sizeof(MeshletInfo) == 48 && sizeof(MeshInfo) == 56);

    struct MeshInstance
    {
        uint64_t name;
        uint32_t minfoId, nodeId;
        donut::math::affine3 transform;
        donut::math::box3 bbox;
        donut::math::float3 center;
        uint32_t padding;
    };
    static_assert(sizeof(MeshInstance) == 104);

    struct MeshNode
    {
        uint64_t name;
        uint32_t parentId, siblingId, instanceId;
        donut::math::affine3 transform;
        donut::math::box3 bbox;
        donut::math::float3 center;
    };
    static_assert(sizeof(MeshNode) == 104);

    struct Material
    {
        uint64_t name;
        uint64_t diffuseTexture;
    };
#pragma pack(pop)

    // Meshlet header as stored in the MESHLET_INFO stream (16 bytes) and consumed by asteroidMS/TS.
    struct MeshletHeader
    {
        uint32_t packedVertexCount;  // [31:24] vertex count, [23:0] 3 x u8 quantized culling data
        uint32_t packedPrimCount;    // [31:24] primitive count, [23:0] 3 x u8 quantized culling data
        uint32_t vertexOffset;       // into the INDEX/UINT32 stream
        uint32_t primOffset;         // byte offset into the INDEX/UINT8 stream

        uint32_t vertexCount() const { return packedVertexCount >> 24; }
        uint32_t primCount() const { return packedPrimCount >> 24; }
    };
    static_assert(sizeof(MeshletHeader) == 16);

    struct StreamView
    {
        StreamType type = StreamType(0);
        StreamVary vary = StreamVary(0);
        StreamSemantic semantic = StreamSemantic(0);
        size_t elemCount = 0;
        size_t elemSize = 0;
        const void* data = nullptr;

        explicit operator bool() const { return data != nullptr; }
    };

    // In-memory view of one MeshSet (equivalent of the 2018 MeshSetBase/MeshletSet).
    class MeshSet
    {
    public:
        MeshSetType type = MeshSetType::Mesh;
        std::string name;
        uint32_t meshletMaxVerts = 0;
        uint32_t meshletMaxPrims = 0;
        donut::math::box3 bbox;

        StreamView positions, texcoords0, texcoords1, normals, tangents, bitangents;
        uint32_t nverts = 0;

        // MeshSetType::Mesh
        StreamView indices;
        std::vector<MeshInfo> meshInfos;

        // MeshSetType::Meshlet
        StreamView indices32;   // per-meshlet vertex index lists
        StreamView indices8;    // meshlet-local triangle indices
        StreamView meshlets;    // MeshletHeader[]
        std::vector<MeshletInfo> meshletInfos;

        std::vector<MeshInstance> instances;
        std::vector<MeshNode> nodes;
        uint32_t rootNodeId = 0;
        std::vector<Material> materials;

        // string-table lookups for the uint64 'name' fields above
        std::vector<std::string> strings;

        // owns the file data referenced by the StreamViews
        nvrhi::AutoPtr<donut::chunk::ChunkFile> source;
        const char* string(uint64_t index) const
        {
            return index < strings.size() ? strings[size_t(index)].c_str() : nullptr;
        }

        const MeshletHeader* meshletHeaders() const { return (const MeshletHeader*)meshlets.data; }
        uint32_t meshletCount() const { return uint32_t(meshlets.elemCount); }
    };

    // Reads the first MeshSet from a .chk blob. Errors are reported via donut::log.
    std::shared_ptr<MeshSet> LoadMeshSet(const nvrhi::AutoPtr<nvrhi::IDataBlob>& blob, const char* path);
}

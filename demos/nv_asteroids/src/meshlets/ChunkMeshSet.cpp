#include "ChunkMeshSet.h"

#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>

#include <cstring>

using namespace donut;

namespace chunk2018
{
namespace
{
    struct StringsTableDesc
    {
        uint32_t flags;
        uint32_t nstrings;
        struct TableEntry { uint64_t offset, length; };
    };

    struct StreamDesc
    {
        uint64_t flags;     // [3:0] type, [5:4] vary, [9:6] semantic
        uint64_t elemCount;
        uint64_t elemSize;
    };

    class Reader
    {
    public:
        Reader(std::shared_ptr<chunk::ChunkFile const> file) : m_File(std::move(file)) { }

        const chunk::Chunk* getChunk(ChunkId id, uint32_t expectedType)
        {
            if (!id.valid())
                return nullptr;
            const chunk::Chunk* c = m_File->getChunk(id);
            if (!c)
            {
                error("chunk not found");
                return nullptr;
            }
            if (c->chunkType != expectedType)
            {
                error("chunk : wrong type %d (expected %d)", c->chunkType, expectedType);
                return nullptr;
            }
            if (c->chunkVersion != c_ChunkVersion)
            {
                error("chunk : wrong version %d (expected %d)", c->chunkVersion, c_ChunkVersion);
                return nullptr;
            }
            if (!c->data || !c->size)
            {
                error("no data in chunk");
                return nullptr;
            }
            return c;
        }

        bool loadStrings(MeshSet& mset)
        {
            std::vector<const chunk::Chunk*> tables;
            m_File->getChunks(CHUNKTYPE_STRINGS_TABLE, tables);
            if (tables.empty())
                return true;
            if (tables.size() > 1)
            {
                error("too many string tables");
                return false;
            }
            const uint8_t* data = (const uint8_t*)tables[0]->data;
            const StringsTableDesc& desc = *(const StringsTableDesc*)data;
            const auto* entries = (const StringsTableDesc::TableEntry*)(data + sizeof(StringsTableDesc));
            const char* chars = (const char*)(entries + desc.nstrings);
            mset.strings.reserve(desc.nstrings);
            for (uint32_t i = 0; i < desc.nstrings; ++i)
                mset.strings.emplace_back(chars + entries[i].offset);
            return true;
        }

        bool loadStream(ChunkId id, StreamType type, StreamVary vary, StreamSemantic semantic, StreamView& out)
        {
            out = StreamView();
            const chunk::Chunk* c = getChunk(id, CHUNKTYPE_STREAM);
            if (!c)
                return false;
            const StreamDesc& desc = *(const StreamDesc*)c->data;
            if ((desc.flags & 0xF) != type)
                return error("datastream chunk : bad type"), false;
            if (((desc.flags >> 4) & 0x3) != vary)
                return error("datastream chunk : bad vertex type"), false;
            if (((desc.flags >> 6) & 0xF) != semantic)
                return error("datastream chunk : bad semantic"), false;
            out.type = type;
            out.vary = vary;
            out.semantic = semantic;
            out.elemCount = size_t(desc.elemCount);
            out.elemSize = size_t(desc.elemSize);
            out.data = (const uint8_t*)c->data + sizeof(StreamDesc);
            return true;
        }

        template <typename... Args>
        void error(const char* fmt, Args... args)
        {
            std::string msg = std::string("ChunkFile '") + m_File->getFilePath() + "' : " + fmt;
            log::error(msg.c_str(), args...);
        }

        std::shared_ptr<chunk::ChunkFile const> m_File;
    };
}

std::shared_ptr<MeshSet> LoadMeshSet(const std::shared_ptr<vfs::IBlob>& blob, const char* path)
{
    auto file = chunk::ChunkFile::deserialize(std::weak_ptr<vfs::IBlob const>(blob), path);
    if (!file)
        return nullptr;

    Reader reader(file);
    auto mset = std::make_shared<MeshSet>();
    if (!reader.loadStrings(*mset))
        return nullptr;

    std::vector<const chunk::Chunk*> meshsets;
    file->getChunks(CHUNKTYPE_MESHSET, meshsets);
    if (meshsets.empty())
        return nullptr;
    const chunk::Chunk* msChunk = reader.getChunk(meshsets[0]->chunkId, CHUNKTYPE_MESHSET);
    if (!msChunk || msChunk->size < sizeof(MeshSetDesc))
        return nullptr;

    MeshSetDesc desc;
    memcpy(&desc, msChunk->data, sizeof(desc));

    const uint32_t typeBits = desc.flags & 0xF;
    if (typeBits > 1)
    {
        reader.error("incorrect Set type (%d)", typeBits);
        return nullptr;
    }
    mset->type = MeshSetType(typeBits);
    mset->name = mset->string(desc.name) ? mset->string(desc.name) : "";
    mset->bbox = desc.bbox;

    auto& ids = desc.streamChunkIds;
    if (!reader.loadStream(ids[MeshSetDesc::POSITIONS], FP32, VERTEX, POSITION, mset->positions))
    {
        reader.error("failed to load set");
        return nullptr;
    }
    mset->nverts = uint32_t(mset->positions.elemCount);
    reader.loadStream(ids[MeshSetDesc::TEXCOORDS0], FP32, VERTEX, TEXCOORD, mset->texcoords0);
    reader.loadStream(ids[MeshSetDesc::TEXCOORDS1], FP32, VERTEX, TEXCOORD, mset->texcoords1);
    reader.loadStream(ids[MeshSetDesc::NORMALS], UINT32, VERTEX, NORMAL, mset->normals);
    reader.loadStream(ids[MeshSetDesc::TANGENTS], UINT32, VERTEX, TANGENT, mset->tangents);
    reader.loadStream(ids[MeshSetDesc::BITANGENTS], UINT32, VERTEX, BITANGENT, mset->bitangents);

    if (mset->type == MeshSetType::Meshlet)
    {
        mset->meshletMaxVerts = desc.meshletMaxVerts;
        mset->meshletMaxPrims = desc.meshletMaxPrims;
        if (!reader.loadStream(ids[MeshSetDesc::MESHLET_INDICES32], UINT32, VARY_NONE, INDEX, mset->indices32) ||
            !reader.loadStream(ids[MeshSetDesc::MESHLET_INDICES8], UINT8, VARY_NONE, INDEX, mset->indices8) ||
            !reader.loadStream(ids[MeshSetDesc::MESHLET_INFO], UINT32, VARY_NONE, MESHLET_INFO, mset->meshlets))
        {
            reader.error("failed to load set");
            return nullptr;
        }
    }
    else
    {
        if (!reader.loadStream(ids[MeshSetDesc::MESH_INDICES], UINT32, VARY_NONE, INDEX, mset->indices))
        {
            reader.error("failed to load set");
            return nullptr;
        }
    }

    if (desc.minfosChunkId.valid())
    {
        const chunk::Chunk* c = reader.getChunk(desc.minfosChunkId, CHUNKTYPE_MESH_INFOS);
        if (!c)
            return nullptr;
        const uint32_t* header = (const uint32_t*)c->data;
        if ((header[0] & 0xF) != typeBits)
        {
            reader.error("incorrect subset type (%d)", header[0] & 0xF);
            return nullptr;
        }
        const uint32_t count = header[1];
        if (mset->type == MeshSetType::Meshlet)
        {
            mset->meshletInfos.resize(count);
            memcpy(mset->meshletInfos.data(), header + 2, sizeof(MeshletInfo) * count);
        }
        else
        {
            mset->meshInfos.resize(count);
            memcpy(mset->meshInfos.data(), header + 2, sizeof(MeshInfo) * count);
        }
    }

    if (desc.instancesChunkId.valid())
    {
        const chunk::Chunk* c = reader.getChunk(desc.instancesChunkId, CHUNKTYPE_MESH_INSTANCES);
        if (!c)
            return nullptr;
        const uint32_t count = *(const uint32_t*)c->data;
        mset->instances.resize(count);
        memcpy(mset->instances.data(), (const uint8_t*)c->data + 4, sizeof(MeshInstance) * count);
    }

    if (desc.nodesChunkId.valid())
    {
        const chunk::Chunk* c = reader.getChunk(desc.nodesChunkId, CHUNKTYPE_MESH_NODES);
        if (!c)
            return nullptr;
        const uint32_t* header = (const uint32_t*)c->data;
        mset->rootNodeId = header[1];
        mset->nodes.resize(header[0]);
        memcpy(mset->nodes.data(), header + 2, sizeof(MeshNode) * header[0]);
    }

    if (desc.materialsChunkId.valid())
    {
        const chunk::Chunk* c = reader.getChunk(desc.materialsChunkId, CHUNKTYPE_MATERIALS);
        if (!c)
            return nullptr;
        const uint32_t count = *(const uint32_t*)c->data;
        mset->materials.resize(count);
        memcpy(mset->materials.data(), (const uint8_t*)c->data + 4, sizeof(Material) * count);
    }

    mset->source = file;
    return mset;
}
}

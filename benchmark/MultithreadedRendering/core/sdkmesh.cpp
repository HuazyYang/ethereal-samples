#include "sdkmesh.h"

#include <cstdio>
#include <cstring>

namespace MtScene
{

bool SdkMesh::Load(const std::filesystem::path& path, std::string& error)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
    {
        error = "cannot open " + path.string();
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0)
    {
        fclose(f);
        error = "empty file " + path.string();
        return false;
    }
    m_Data.resize(size_t(size));
    const size_t read = fread(m_Data.data(), 1, m_Data.size(), f);
    fclose(f);
    if (read != m_Data.size())
    {
        error = "short read on " + path.string();
        return false;
    }
    m_Directory = path.parent_path();

    if (m_Data.size() < sizeof(SdkMeshHeader))
    {
        error = "file too small for an SDKMESH header";
        return false;
    }
    m_Header = reinterpret_cast<const SdkMeshHeader*>(m_Data.data());
    if (m_Header->Version != SDKMESH_FILE_VERSION)
    {
        error = "unsupported SDKMESH version " + std::to_string(m_Header->Version);
        return false;
    }
    const uint64_t staticSize = m_Header->HeaderSize + m_Header->NonBufferDataSize;
    if (staticSize + m_Header->BufferDataSize > m_Data.size())
    {
        error = "SDKMESH section sizes exceed the file size";
        return false;
    }
    auto section = [&](uint64_t offset, size_t count, size_t elementSize, const char* what) -> const uint8_t* {
        if (offset + count * elementSize > staticSize)
        {
            error = std::string("SDKMESH ") + what + " section out of bounds";
            return nullptr;
        }
        return m_Data.data() + offset;
    };
    const uint8_t* p;
    if (!(p = section(m_Header->VertexStreamHeadersOffset, m_Header->NumVertexBuffers, sizeof(SdkMeshVertexBufferHeader), "vertex buffer headers"))) return false;
    m_VertexBuffers = reinterpret_cast<const SdkMeshVertexBufferHeader*>(p);
    if (!(p = section(m_Header->IndexStreamHeadersOffset, m_Header->NumIndexBuffers, sizeof(SdkMeshIndexBufferHeader), "index buffer headers"))) return false;
    m_IndexBuffers = reinterpret_cast<const SdkMeshIndexBufferHeader*>(p);
    if (!(p = section(m_Header->MeshDataOffset, m_Header->NumMeshes, sizeof(SdkMeshMesh), "meshes"))) return false;
    m_Meshes = reinterpret_cast<const SdkMeshMesh*>(p);
    if (!(p = section(m_Header->SubsetDataOffset, m_Header->NumTotalSubsets, sizeof(SdkMeshSubset), "subsets"))) return false;
    m_Subsets = reinterpret_cast<const SdkMeshSubset*>(p);
    if (!(p = section(m_Header->FrameDataOffset, m_Header->NumFrames, sizeof(SdkMeshFrame), "frames"))) return false;
    m_Frames = reinterpret_cast<const SdkMeshFrame*>(p);
    if (!(p = section(m_Header->MaterialDataOffset, m_Header->NumMaterials, sizeof(SdkMeshMaterial), "materials"))) return false;
    m_Materials = reinterpret_cast<const SdkMeshMaterial*>(p);

    // Buffer data: DataOffset is relative to the start of the file (DXUT subtracts the static size and
    // adds it back onto the buffer data pointer).
    m_VertexData.resize(m_Header->NumVertexBuffers);
    for (uint32_t i = 0; i < m_Header->NumVertexBuffers; ++i)
    {
        const auto& vb = m_VertexBuffers[i];
        if (vb.DataOffset < staticSize || vb.DataOffset + vb.SizeBytes > m_Data.size())
        {
            error = "SDKMESH vertex buffer " + std::to_string(i) + " out of bounds";
            return false;
        }
        m_VertexData[i] = m_Data.data() + vb.DataOffset;
    }
    m_IndexData.resize(m_Header->NumIndexBuffers);
    for (uint32_t i = 0; i < m_Header->NumIndexBuffers; ++i)
    {
        const auto& ib = m_IndexBuffers[i];
        if (ib.DataOffset < staticSize || ib.DataOffset + ib.SizeBytes > m_Data.size())
        {
            error = "SDKMESH index buffer " + std::to_string(i) + " out of bounds";
            return false;
        }
        m_IndexData[i] = m_Data.data() + ib.DataOffset;
    }
    for (uint32_t i = 0; i < m_Header->NumMeshes; ++i)
    {
        const auto& mesh = m_Meshes[i];
        if (mesh.NumVertexBuffers != 1 || mesh.VertexBuffers[0] >= m_Header->NumVertexBuffers || mesh.IndexBuffer >= m_Header->NumIndexBuffers ||
            mesh.SubsetOffset + uint64_t(mesh.NumSubsets) * sizeof(uint32_t) > staticSize)
        {
            error = "SDKMESH mesh " + std::to_string(i) + " has an unsupported layout";
            return false;
        }
    }
    return true;
}

const SdkMeshSubset& SdkMesh::Subset(const SdkMeshMesh& mesh, uint32_t subsetInMesh) const
{
    const uint32_t* subsetIndices = reinterpret_cast<const uint32_t*>(m_Data.data() + mesh.SubsetOffset);
    return m_Subsets[subsetIndices[subsetInMesh]];
}

void SdkMesh::VisitFrame(uint32_t frame, std::vector<uint32_t>& order) const
{
    // Iterative version of CDXUTSDKMesh::RenderFrame: mesh, then children, then siblings.
    while (frame != SDKMESH_INVALID_FRAME && frame < m_Header->NumFrames)
    {
        const SdkMeshFrame& f = m_Frames[frame];
        if (f.Mesh != SDKMESH_INVALID_MESH && f.Mesh < m_Header->NumMeshes)
            order.push_back(f.Mesh);
        if (f.ChildFrame != SDKMESH_INVALID_FRAME)
            VisitFrame(f.ChildFrame, order);
        frame = f.SiblingFrame;
    }
}

std::vector<uint32_t> SdkMesh::RenderOrder() const
{
    std::vector<uint32_t> order;
    order.reserve(m_Header->NumMeshes);
    if (m_Header->NumFrames > 0)
        VisitFrame(0, order);
    return order;
}

void SdkMesh::CountDraws(uint64_t& draws, uint64_t& indices) const
{
    draws = indices = 0;
    for (uint32_t meshIndex : RenderOrder())
    {
        const SdkMeshMesh& mesh = m_Meshes[meshIndex];
        for (uint32_t s = 0; s < mesh.NumSubsets; ++s)
        {
            ++draws;
            indices += Subset(mesh, s).IndexCount;
        }
    }
}

uint64_t SdkMesh::ContentHash() const
{
    uint64_t hash = 14695981039346656037ull;
    auto mix = [&](const void* data, size_t size) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * 1099511628211ull;
    };
    for (uint32_t i = 0; i < m_Header->NumVertexBuffers; ++i)
        mix(m_VertexData[i], size_t(m_VertexBuffers[i].SizeBytes));
    for (uint32_t i = 0; i < m_Header->NumIndexBuffers; ++i)
        mix(m_IndexData[i], size_t(m_IndexBuffers[i].SizeBytes));
    for (uint32_t i = 0; i < m_Header->NumMaterials; ++i)
    {
        mix(m_Materials[i].DiffuseTexture, strnlen(m_Materials[i].DiffuseTexture, MAX_TEXTURE_NAME));
        mix(m_Materials[i].NormalTexture, strnlen(m_Materials[i].NormalTexture, MAX_TEXTURE_NAME));
    }
    const std::vector<uint32_t> order = RenderOrder();
    mix(order.data(), order.size() * sizeof(uint32_t));
    return hash;
}

} // namespace MtScene

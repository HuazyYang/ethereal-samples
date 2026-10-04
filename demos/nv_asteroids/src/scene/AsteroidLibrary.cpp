#include "scene/AsteroidLibrary.h"

#include "meshlets/ChunkMeshSet.h"
#include "meshlets/NvMeshShaderPsoExt.h"

#include <donut/core/json.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/ThreadPool.h>

#include <json/json.h>

#include <PxPhysics.h>
#include <cooking/PxCooking.h>
#include <cooking/PxTriangleMeshDesc.h>
#include <geometry/PxTriangleMesh.h>

#include <algorithm>
#include <cfloat>

using namespace donut;
using namespace donut::math;

namespace
{
    // Meshlet limits the asset pipeline and the shaders were built for (2018 globals 0x1402D2528 / 0x1402D252C).
    constexpr uint32_t c_MeshletMaxVerts = nvmesh2018::c_MeshletMaxVerts;
    constexpr uint32_t c_MeshletMaxPrims = nvmesh2018::c_MeshletMaxPrims;

    AsteroidBoundingBox MakeBoundingBox(const box3& box)
    {
        return { float4(box.m_mins, 1.f), float4(box.m_maxs, 1.f) };
    }

    // deviation: the 2018 buffers had no view flags. donut's nvrhi needs explicit strides/view
    // permissions for the structured / byte-address SRVs the meshlet shaders declare, and uses
    // keepInitialState instead of the 2018 begin/endTrackingBufferState calls.
    nvrhi::BufferHandle CreateSrvBuffer(nvrhi::IDevice* device, const char* name, uint64_t byteSize, uint32_t stride)
    {
        if (byteSize == 0)
            return nullptr;

        nvrhi::BufferDesc desc;
        desc.byteSize = (byteSize + 3) & ~uint64_t(3);
        desc.structStride = stride;
        desc.canHaveRawViews = true;
        desc.canHaveTypedViews = true;
        desc.debugName = name;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        return device->createBuffer(desc);
    }

    void WriteRange(nvrhi::ICommandList* commandList, nvrhi::IBuffer* buffer, const void* data,
        size_t elementSize, uint32_t count, uint32_t elementOffset)
    {
        if (!buffer || !data || count == 0)
            return;
        commandList->writeBuffer(buffer, data, elementSize * count, uint64_t(elementSize) * elementOffset);
    }
}

// ------------------------------------------------------------------------------------------------
// AsteroidLod

bool AsteroidLod::Load(vfs::IFileSystem& fs, const std::filesystem::path& path)
{
    // Asteroids.exe: 0x1400111C0
    auto blob = fs.readFile(path);
    if (!blob)
    {
        log::error("Couldn't read file `%s`", path.generic_string().c_str());
        return false;
    }

    meshSet = chunk2018::LoadMeshSet(blob, path.generic_string().c_str());
    if (!meshSet)
    {
        log::error("Unable to load chunk file : `%s`", path.generic_string().c_str());
        return false;
    }
    if (meshSet->meshletMaxVerts != c_MeshletMaxVerts || meshSet->meshletMaxPrims != c_MeshletMaxPrims)
    {
        log::error("Chunk file has wrong maxVerts/maxPrims meshlets : `%s`", path.generic_string().c_str());
        return false;
    }
    if (meshSet->type != chunk2018::MeshSetType::Meshlet)
    {
        log::error("Chunk file is not meshlets type : `%s`", path.generic_string().c_str());
        return false;
    }

    numVertices = meshSet->nverts;
    numIndices = uint32_t(meshSet->indices32.elemCount);
    numPrims = uint32_t(meshSet->indices8.elemCount);
    numMeshlets = meshSet->meshletCount();
    bounds = meshSet->bbox;
    return true;
}

// ------------------------------------------------------------------------------------------------
// AsteroidType

AsteroidType::AsteroidType(uint32_t numLods)
{
    // Asteroids.exe: 0x14000D9C0
    m_Lods.resize(numLods);
    for (auto& lod : m_Lods)
        lod = std::make_unique<AsteroidLod>();
    m_LodInfos.resize(numLods);
    m_ObjectConstants.lodBias = 0.f;
}

AsteroidType::~AsteroidType()
{
    // deviation: the 2018 destructor never released the cooked collision mesh.
    if (m_CollisionMesh)
        m_CollisionMesh->release();
}

void AsteroidType::Load(const AsteroidTypeDesc& desc, const AsteroidLoadContext& context)
{
    // Asteroids.exe: 0x14000E420 (task body)
    m_ObjectConstants.lodBias = desc.lodBias;
    m_ObjectConstants.maxLevelToRender = float(desc.maxLevelToRender);
    m_Name = desc.name;

    const bool haveMaterials = !desc.materialFile.empty();
    if (haveMaterials)
        LoadMaterials(*context.fs, desc.materialFile, context);

    for (const AsteroidLodDesc& lodDesc : desc.lods)
    {
        // deviation: the 2018 code indexes the LOD array without a range check.
        if (lodDesc.level < 0 || size_t(lodDesc.level) >= m_Lods.size())
        {
            log::warning("Asteroid type %s: LOD level %d is out of range", m_Name.c_str(), lodDesc.level);
            continue;
        }

        AsteroidLod& lod = *m_Lods[lodDesc.level];
        lod.Load(*context.fs, lodDesc.model);
        if (!lod.meshSet)
            continue;

        if (!haveMaterials)
        {
            log::error("Materials not supported in chunk files yet");
            break;
        }

        ResolveMaterials(*lod.meshSet);
        for (const chunk2018::MeshletInfo& info : lod.meshSet->meshletInfos)
        {
            if (info.materialId < m_Materials.size())
                lod.material = m_Materials[info.materialId];
        }

        if (context.progress)
            ++context.progress->completed;
    }

    // deviation: guard the collision LOD index (the 2018 code does not).
    if (desc.collisionLod >= 0 && size_t(desc.collisionLod) < m_Lods.size())
        CreateCollisionMesh(*m_Lods[desc.collisionLod], context);

    ComputeBoundsAndLodInfos();
}

void AsteroidType::LoadMaterials(vfs::IFileSystem& fs, const std::filesystem::path& materialFile,
    const AsteroidLoadContext& context)
{
    // Asteroids.exe: 0x1400119C0. Texture paths are relative to the media root, i.e. two levels up
    // from <media>/Clustered/materials.json.
    const std::filesystem::path basePath = materialFile.parent_path().parent_path();

    MaterialLoadParams params;
    params.textureCache = context.textureCache;
    params.threadPool = context.threadPool;
    LoadMaterialsFile(fs, materialFile, basePath, params, m_MaterialsByName);
}

void AsteroidType::ResolveMaterials(const chunk2018::MeshSet& meshSet)
{
    // Asteroids.exe: 0x14000E9F0 (only effective for the first LOD that has more materials)
    if (m_Materials.size() >= meshSet.materials.size())
        return;

    m_Materials.resize(meshSet.materials.size(), nullptr);
    for (size_t i = 0; i < meshSet.materials.size(); ++i)
    {
        const char* name = meshSet.string(meshSet.materials[i].name);
        if (!name)
        {
            log::warning("Material %d doesn't have a name", int(i));
            continue;
        }

        auto it = m_MaterialsByName.find(name);
        if (it == m_MaterialsByName.end())
        {
            log::warning("Couldn't find a json entry for material %s", name);
            continue;
        }
        m_Materials[i] = it->second.get();
    }
}

void AsteroidType::CreateCollisionMesh(const AsteroidLod& lod, const AsteroidLoadContext& context)
{
    // Asteroids.exe: 0x14000E710 — triangle mesh from the collision LOD, cooked with default params.
    if (!lod.meshSet || !context.cooking || !context.physics)
        return;

    const chunk2018::MeshSet& ms = *lod.meshSet;
    const chunk2018::MeshletHeader* meshlets = ms.meshletHeaders();
    const uint32_t* vertexIndices = static_cast<const uint32_t*>(ms.indices32.data);
    const uint8_t* primIndices = static_cast<const uint8_t*>(ms.indices8.data);
    if (!meshlets || !vertexIndices || !primIndices || !ms.positions)
        return;

    uint32_t numTris = 0;
    for (uint32_t m = 0; m < ms.meshletCount(); ++m)
        numTris += meshlets[m].primCount();

    std::vector<uint32_t> indices;
    indices.reserve(size_t(numTris) * 3);
    for (uint32_t m = 0; m < ms.meshletCount(); ++m)
    {
        const chunk2018::MeshletHeader& meshlet = meshlets[m];
        const uint32_t* vtx = vertexIndices + meshlet.vertexOffset;
        const uint8_t* prim = primIndices + meshlet.primOffset;
        for (uint32_t t = 0; t < meshlet.primCount(); ++t, prim += 3)
        {
            indices.push_back(vtx[prim[0]]);
            indices.push_back(vtx[prim[1]]);
            indices.push_back(vtx[prim[2]]);
        }
    }

    physx::PxTriangleMeshDesc meshDesc;
    meshDesc.points.count = ms.nverts;
    meshDesc.points.stride = sizeof(float3);
    meshDesc.points.data = ms.positions.data;
    meshDesc.triangles.count = numTris;
    meshDesc.triangles.stride = 3 * sizeof(uint32_t);
    meshDesc.triangles.data = indices.data();

    m_CollisionMesh = context.cooking->createTriangleMesh(meshDesc, context.physics->getPhysicsInsertionCallback());
}

void AsteroidType::ComputeBoundsAndLodInfos()
{
    // Asteroids.exe: 0x14000ECD0
    for (size_t k = 0; k < m_Lods.size(); ++k)
    {
        const AsteroidLod& lod = *m_Lods[k];
        AsteroidLODInfo& info = m_LodInfos[k];
        if (k == 0)
        {
            info.indexStart = info.vertexStart = info.primStart = info.minfoStart = 0;
        }
        else
        {
            const AsteroidLODInfo& prev = m_LodInfos[k - 1];
            info.vertexStart = prev.vertexStart + prev.numVertices;
            info.indexStart = prev.indexStart + prev.numIndices;
            info.primStart = prev.primStart + prev.numPrims;
            info.minfoStart = prev.minfoStart + prev.numMinfo;
        }
        info.numVertices = lod.numVertices;
        info.numIndices = lod.numIndices;
        info.numPrims = lod.numPrims;
        info.numMinfo = lod.numMeshlets;

        m_Bounds |= lod.bounds;
        if (k < c_MaxAsteroidLods)
            m_ObjectConstants.bboxLods[k] = MakeBoundingBox(lod.bounds);
    }

    if (!m_LodInfos.empty())
    {
        const AsteroidLODInfo& last = m_LodInfos.back();
        m_TotalVertices = last.vertexStart + last.numVertices;
        m_TotalIndices = last.indexStart + last.numIndices;
        m_TotalPrims = last.primStart + last.numPrims;
        m_TotalMeshlets = last.minfoStart + last.numMinfo;
    }

    m_ObjectConstants.bbox = MakeBoundingBox(m_Bounds);
    m_ObjectConstants.center = m_Bounds.m_mins + (m_Bounds.m_maxs - m_Bounds.m_mins) * 0.5f;
    m_ObjectConstants.radius = length(m_Bounds.m_maxs - m_Bounds.m_mins) * 0.5f;

    for (size_t k = m_Lods.size(); k < c_MaxAsteroidLods; ++k)
    {
        m_ObjectConstants.bboxLods[k].bboxMin = float4(FLT_MAX, FLT_MAX, FLT_MAX, 1.f);
        m_ObjectConstants.bboxLods[k].bboxMax = float4(-FLT_MAX, -FLT_MAX, -FLT_MAX, 1.f);
    }
}

bool AsteroidType::CreateBuffers(nvrhi::IDevice* device)
{
    // Asteroids.exe: 0x14000F120
    m_PositionsBuffer = CreateSrvBuffer(device, "Vertex Positions", uint64_t(m_TotalVertices) * 12, 12);
    m_NormalsBuffer = CreateSrvBuffer(device, "Vertex Normals", uint64_t(m_TotalVertices) * 4, 4);
    m_Texcoord1Buffer = CreateSrvBuffer(device, "Vertex Texcoord1", uint64_t(m_TotalVertices) * 8, 8);
    m_Texcoord2Buffer = CreateSrvBuffer(device, "Vertex Texcoord2", uint64_t(m_TotalVertices) * 8, 8);
    m_TangentsBuffer = CreateSrvBuffer(device, "Vertex Tangents", uint64_t(m_TotalVertices) * 4, 4);
    m_BitangentsBuffer = CreateSrvBuffer(device, "Vertex Bitangents", uint64_t(m_TotalVertices) * 4, 4);
    m_VertexIndicesBuffer = CreateSrvBuffer(device, "Vertex Indices", uint64_t(m_TotalIndices) * 4, 4);
    m_TriangleIndicesBuffer = CreateSrvBuffer(device, "Triangle Indices", m_TotalPrims, 0);
    m_MeshletInfoBuffer = CreateSrvBuffer(device, "Meshlet Info", uint64_t(m_TotalMeshlets) * 16, 16);
    m_LodInfoBuffer = CreateSrvBuffer(device, "LOD Info", m_LodInfos.size() * sizeof(AsteroidLODInfo), sizeof(AsteroidLODInfo));

    nvrhi::BufferDesc constantsDesc;
    constantsDesc.byteSize = sizeof(AsteroidObjectConstants);
    constantsDesc.debugName = "Object Constants";
    constantsDesc.isConstantBuffer = true;
    constantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    constantsDesc.keepInitialState = true;
    m_ObjectConstantsBuffer = device->createBuffer(constantsDesc);

    return m_PositionsBuffer && m_NormalsBuffer && m_Texcoord1Buffer && m_Texcoord2Buffer && m_TangentsBuffer
        && m_BitangentsBuffer && m_VertexIndicesBuffer && m_TriangleIndicesBuffer && m_MeshletInfoBuffer && m_LodInfoBuffer;
}

void AsteroidType::CreateRenderResources(nvrhi::ICommandList* commandList, engine::CommonRenderPasses& commonPasses,
    nvrhi::IBindingLayout* materialBindingLayout)
{
    // Asteroids.exe: 0x140012770
    nvrhi::IDevice* device = commandList->getDevice();

    if (!CreateBuffers(device))
        log::error("Buffer creation failed");

    // Asteroids.exe: 0x14000FB90
    for (SceneMaterial* material : m_Materials)
    {
        // deviation: null check (unresolved chunk materials crash the 2018 code).
        if (material)
            material->CreateResources(device, commandList, commonPasses, materialBindingLayout, (m_AttributeFlags & 0x40) != 0);
    }

    for (size_t i = 0; i < m_Lods.size(); ++i)
    {
        const AsteroidLod& lod = *m_Lods[i];
        if (!lod.meshSet)
            continue;

        const chunk2018::MeshSet& ms = *lod.meshSet;
        const AsteroidLODInfo& info = m_LodInfos[i];

        WriteRange(commandList, m_PositionsBuffer, ms.positions.data, 12, lod.numVertices, info.vertexStart);
        WriteRange(commandList, m_Texcoord1Buffer, ms.texcoords0.data, 8, lod.numVertices, info.vertexStart);
        WriteRange(commandList, m_Texcoord2Buffer, ms.texcoords1.data, 8, lod.numVertices, info.vertexStart);
        WriteRange(commandList, m_NormalsBuffer, ms.normals.data, 4, lod.numVertices, info.vertexStart);
        WriteRange(commandList, m_TangentsBuffer, ms.tangents.data, 4, lod.numVertices, info.vertexStart);
        WriteRange(commandList, m_BitangentsBuffer, ms.bitangents.data, 4, lod.numVertices, info.vertexStart);
        WriteRange(commandList, m_VertexIndicesBuffer, ms.indices32.data, 4, lod.numIndices, info.indexStart);
        WriteRange(commandList, m_TriangleIndicesBuffer, ms.indices8.data, 1, lod.numPrims, info.primStart);
        WriteRange(commandList, m_MeshletInfoBuffer, ms.meshlets.data, 16, lod.numMeshlets, info.minfoStart);
    }

    if (m_LodInfoBuffer && !m_LodInfos.empty())
        commandList->writeBuffer(m_LodInfoBuffer, m_LodInfos.data(), m_LodInfos.size() * sizeof(AsteroidLODInfo));

    if (m_ObjectConstantsBuffer)
        commandList->writeBuffer(m_ObjectConstantsBuffer, &m_ObjectConstants, sizeof(m_ObjectConstants));

    // The 2018 code drops the chunk files here (it overwrote the pointer without deleting it).
    for (auto& lod : m_Lods)
        lod->meshSet.reset();
}

void AsteroidType::UpdateObjectConstants(nvrhi::ICommandList* commandList)
{
    if (m_ObjectConstantsBuffer)
        commandList->writeBuffer(m_ObjectConstantsBuffer, &m_ObjectConstants, sizeof(m_ObjectConstants));
}

nvrhi::IBuffer* AsteroidType::GetBuffer(AsteroidBufferType type) const
{
    // Asteroids.exe: 0x140010060
    switch (type)
    {
    case AsteroidBufferType::VertexPositions: return m_PositionsBuffer;
    case AsteroidBufferType::VertexNormals: return m_NormalsBuffer;
    case AsteroidBufferType::VertexTexcoord1: return m_Texcoord1Buffer;
    case AsteroidBufferType::VertexTexcoord2: return m_Texcoord2Buffer;
    case AsteroidBufferType::VertexTangents: return m_TangentsBuffer;
    case AsteroidBufferType::VertexBitangents: return m_BitangentsBuffer;
    case AsteroidBufferType::VertexIndices: return m_VertexIndicesBuffer;
    case AsteroidBufferType::TriangleIndices: return m_TriangleIndicesBuffer;
    case AsteroidBufferType::MeshletInfo: return m_MeshletInfoBuffer;
    case AsteroidBufferType::LodInfo: return m_LodInfoBuffer;
    default:
        log::error("Incorrect buffer type requested");
        return nullptr;
    }
}

// ------------------------------------------------------------------------------------------------
// AsteroidLibrary

bool AsteroidLibrary::Load(const Json::Value& root, const std::filesystem::path& mediaPath, const AsteroidLoadContext& context)
{
    // Asteroids.exe: 0x1400101C0
    if (!root.isArray())
    {
        log::error("Expected an array at the top level, in the LOD file");
        return false;
    }

    uint32_t typeId = 0;
    for (const Json::Value& node : root)
    {
        const std::string name = json::Read<std::string>(node["name"], "default-asteroid");
        const bool duplicate = std::any_of(m_Types.begin(), m_Types.end(),
            [&name](const std::shared_ptr<AsteroidType>& type) { return type->GetName() == name; });
        if (duplicate)
        {
            log::warning("duplicate asteroid type in asteroids file");
            continue;
        }

        AsteroidTypeDesc desc;
        desc.name = name;
        desc.maxLevelToRender = json::Read<int>(node["maxLevelToRender"], 9);
        desc.materialFile = mediaPath / std::filesystem::path(json::Read<std::string>(node["materialFile"], "Clustered/materials.mtl"));
        desc.lodBias = json::Read<float>(node["lodBias"], 0.f);
        desc.typeId = typeId++;
        desc.collisionLod = json::Read<int>(node["collisionLod"], 5);
        for (const Json::Value& lodNode : node["lods"])
        {
            AsteroidLodDesc lodDesc;
            lodDesc.level = json::Read<int>(lodNode["level"], 0);
            lodDesc.model = mediaPath / std::filesystem::path(json::Read<std::string>(lodNode["model"], "placeholder-asteroid"));
            desc.lods.push_back(std::move(lodDesc));
        }

        auto type = std::make_shared<AsteroidType>(uint32_t(desc.lods.size()));
        m_Types.push_back(type);

        // deviation: the 2018 code ran each type on a concurrency::task_group; donut's ThreadPool is
        // used when available (it also decodes the textures), otherwise the types load serially.
        if (context.threadPool)
            context.threadPool->AddTask([type, desc, context]() { type->Load(desc, context); });
        else
            type->Load(desc, context);
    }

    if (context.threadPool)
        context.threadPool->WaitForTasks();

    return true;
}

void AsteroidLibrary::CreateRenderResources(nvrhi::IDevice* device, engine::CommonRenderPasses& commonPasses,
    nvrhi::IBindingLayout* materialBindingLayout)
{
    // Asteroids.exe: part of SpaceScene::vfunc03 (0x14005B440) — one command list submission per type.
    nvrhi::CommandListHandle commandList = device->createCommandList();
    for (const auto& type : m_Types)
    {
        commandList->open();
        type->CreateRenderResources(commandList, commonPasses, materialBindingLayout);
        commandList->close();
        device->executeCommandList(commandList);
        device->waitForIdle();
    }
}

void AsteroidLibrary::UpdateObjectConstants(nvrhi::ICommandList* commandList)
{
    for (uint32_t typeId : m_DirtyTypeIds)
    {
        if (typeId < m_Types.size())
            m_Types[typeId]->UpdateObjectConstants(commandList);
    }
    m_DirtyTypeIds.clear();
}

void AsteroidLibrary::MarkObjectConstantsDirty(uint32_t typeId)
{
    // unresolved: no writer of the 2018 dirty list was found (likely an inlined UI setter).
    if (std::find(m_DirtyTypeIds.begin(), m_DirtyTypeIds.end(), typeId) == m_DirtyTypeIds.end())
        m_DirtyTypeIds.push_back(typeId);
}

uint32_t AsteroidLibrary::GetAsteroidTypeId(const std::string& name) const
{
    // Asteroids.exe: 0x14000FF60
    for (size_t i = 0; i < m_Types.size(); ++i)
    {
        if (m_Types[i]->GetName() == name)
            return uint32_t(i);
    }
    log::error("Asteroid type with name = %s does not exist", name.c_str());
    return c_InvalidAsteroidTypeId;
}

std::shared_ptr<AsteroidType> AsteroidLibrary::GetAsteroidType(uint32_t typeId) const
{
    // Asteroids.exe: 0x14000FEF0 / 0x1400144F0. deviation: the 2018 bounds check used '<='.
    if (typeId < m_Types.size())
        return m_Types[typeId];
    log::error("Asteroid type with typeId = %u does not exist", typeId);
    return nullptr;
}

nvrhi::IBuffer* AsteroidLibrary::GetBuffer(uint32_t typeId, AsteroidBufferType type) const
{
    auto asteroidType = GetAsteroidType(typeId);
    return asteroidType ? asteroidType->GetBuffer(type) : nullptr;
}

nvrhi::IBuffer* AsteroidLibrary::GetObjectConstantsBuffer(uint32_t typeId) const
{
    auto asteroidType = GetAsteroidType(typeId);
    return asteroidType ? asteroidType->GetObjectConstantsBuffer() : nullptr;
}

SceneMaterial* AsteroidLibrary::GetMaterial(uint32_t typeId) const
{
    auto asteroidType = GetAsteroidType(typeId);
    return asteroidType ? asteroidType->GetMaterial() : nullptr;
}

box3 AsteroidLibrary::GetBounds(uint32_t typeId) const
{
    // Asteroids.exe: lambda 0x14005B0D0 (logs 0x140060260 when the type cannot be locked)
    auto asteroidType = GetAsteroidType(typeId);
    if (asteroidType)
        return asteroidType->GetBounds();
    log::error("Asteroid type with typeId = %u could not be locked", typeId);
    return box3::empty();
}

uint32_t AsteroidLibrary::GetNumPrims(uint32_t typeId) const
{
    auto asteroidType = GetAsteroidType(typeId);
    return asteroidType ? asteroidType->GetNumPrims() : ~0u;
}

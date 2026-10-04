#include "scene/SpaceObject.h"
#include "scene/AsteroidLibrary.h"

#include "meshlets/ChunkMeshSet.h"
#include "meshlets/NvMeshShaderPsoExt.h"

#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/TextureCache.h>
#include <donut/engine/ThreadPool.h>

#include <assimp/cimport.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <sstream>

using namespace donut;
using namespace donut::math;

namespace
{
    uint32_t PackSnorm8(const aiVector3D& v)
    {
        // 2018: truncation toward zero, w byte = 0 (a zero vector yields NaN -> 0x80000000 -> byte 0).
        const float s = 127.f / sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
        return uint32_t(uint8_t(int(v.x * s))) | (uint32_t(uint8_t(int(v.y * s))) << 8) | (uint32_t(uint8_t(int(v.z * s))) << 16);
    }

    affine3 ConvertTransform(const aiMatrix4x4& m)
    {
        // aiMatrix4x4 is column-vector; donut's affine3 is row-vector, so transpose.
        return affine3(
            m.a1, m.b1, m.c1,
            m.a2, m.b2, m.c2,
            m.a3, m.b3, m.c3,
            m.a4, m.b4, m.c4);
    }

    // deviation: 2018 SRV buffers had no view flags and were transitioned with begin/endTracking.
    template<typename T>
    nvrhi::BufferHandle CreateStaticBuffer(nvrhi::IDevice* device, nvrhi::ICommandList* commandList,
        const T* data, size_t count, const char* name, uint32_t stride = sizeof(T))
    {
        if (!data || count == 0)
            return nullptr;

        const size_t byteSize = count * sizeof(T);
        nvrhi::BufferDesc desc;
        desc.byteSize = (byteSize + 3) & ~size_t(3);
        desc.structStride = stride;
        desc.canHaveRawViews = true;
        desc.canHaveTypedViews = true;
        desc.debugName = name;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        nvrhi::BufferHandle buffer;
        device->createBuffer(desc, &buffer);
        if (buffer)
            commandList->writeBuffer(buffer, data, byteSize);
        return buffer;
    }

    template<typename T>
    void CreateVertexBuffer(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, uint32_t attribute,
        uint32_t enabledAttributes, SceneBufferGroup& buffers, std::vector<T>& data, const char* name)
    {
        // Asteroids.exe: 0x14004D200 / 0x14004D3B0 / 0x14004D560
        if (!(attribute & enabledAttributes) || data.empty())
            return;

        nvrhi::BufferDesc desc;
        desc.byteSize = data.size() * sizeof(T);
        desc.debugName = name;
        desc.isVertexBuffer = true;
        desc.initialState = nvrhi::ResourceStates::VertexBuffer;
        desc.keepInitialState = true;
        nvrhi::BufferHandle buffer;
        device->createBuffer(desc, &buffer);
        buffers.vertexBuffers[int(attribute)] = buffer;
        commandList->writeBuffer(buffer, data.data(), desc.byteSize);
        std::vector<T>().swap(data);
    }
}

SpaceObject::SpaceObject(nvrhi::AutoPtr<vfs::IFileSystem> fs)
    : m_FS(std::move(fs))
{
    // Asteroids.exe: 0x140050520
    m_RootNode = std::make_shared<SceneNode>(nullptr, "SpaceObjectRoot", nullptr);
}

SpaceObject::~SpaceObject()
{
    // Asteroids.exe: 0x140050EB0. deviation: materials are owned by the map / imported list instead of
    // being deleted through the index vector (which could double-free shared entries).
    ClearCpuData();
    for (SceneMeshInstance* instance : m_MeshInstances)
        delete instance;
    for (SceneMesh* mesh : m_Meshes)
        delete mesh;
}

bool SpaceObject::Load(const std::filesystem::path& file, const std::filesystem::path& materialsFile,
    const SpaceObjectLoadContext& context, bool instanceAtMeshlessNodes, uint32_t loadFlags)
{
    // Asteroids.exe: vfunc04 0x140054DE0
    m_Directory = file.parent_path();
    m_LoadFlags = loadFlags;

    const bool hasMaterials = !materialsFile.empty();
    if (hasMaterials)
        LoadMaterialsJson(materialsFile, context);

    // Case-sensitive comparison, like std::filesystem::path::compare in the binary.
    if (file.extension().compare(std::filesystem::path(".fbx")) != 0)
        return LoadChunkFile(file, hasMaterials);

    return LoadAssimp(file, hasMaterials, context, instanceAtMeshlessNodes, loadFlags);
}

void SpaceObject::LoadMaterialsJson(const std::filesystem::path& materialsFile, const SpaceObjectLoadContext& context)
{
    // Asteroids.exe: 0x140056FD0. Texture names are relative to the model's directory.
    MaterialLoadParams params;
    params.textureCache = context.textureCache;
    params.threadPool = context.threadPool;
    LoadMaterialsFile(*m_FS, materialsFile, m_Directory, params, m_MaterialsByName);
}

bool SpaceObject::LoadAssimp(const std::filesystem::path& file, bool hasMaterialsJson, const SpaceObjectLoadContext& context,
    bool instanceAtMeshlessNodes, uint32_t loadFlags)
{
    // Asteroids.exe: 0x140055030
    unsigned int postProcess = 0;
    if (loadFlags & LoadFlag_JoinIdenticalVertices) postProcess |= aiProcess_JoinIdenticalVertices;
    if (loadFlags & LoadFlag_Triangulate) postProcess |= aiProcess_Triangulate;
    if (loadFlags & LoadFlag_CalcTangentSpace) postProcess |= aiProcess_CalcTangentSpace;
    if (loadFlags & LoadFlag_GenNormals) postProcess |= aiProcess_GenNormals;
    if (loadFlags & LoadFlag_FlipUVs) postProcess |= aiProcess_FlipUVs;

    nvrhi::AutoPtr<nvrhi::IDataBlob> blob;
    if (NVRHI_FAILED(m_FS->readFile(file, &blob)) || !blob)
    {
        log::error("Couldn't read file `%s`", file.generic_string().c_str());
        return false;
    }

    const std::string hint = file.extension().string();
    const aiScene* scene = aiImportFileFromMemory(static_cast<const char*>(blob->GetDataPtr()),
        unsigned(blob->GetSize()), postProcess, hint.c_str());
    if (!scene)
    {
        log::error("Unable to load scene file `%s`: %s", file.generic_string().c_str(), aiGetErrorString());
        return false;
    }

    if (hasMaterialsJson)
        ResolveMaterials(scene);
    else
        ImportAssimpMaterials(scene, context);

    CreateMeshesFromAssimp(scene);
    ProcessNode(scene, scene->mRootNode, m_RootNode, affine3::identity(), 0, instanceAtMeshlessNodes);
    UpdateBounds();

    aiReleaseImport(scene);
    return true;
}

void SpaceObject::ImportAssimpMaterials(const aiScene* scene, const SpaceObjectLoadContext& context)
{
    // Asteroids.exe: 0x140056480
    auto loadTexture = [this, &context](const aiString& name, bool sRGB) -> nvrhi::AutoPtr<engine::LoadedTexture>
    {
        if (!context.textureCache)
            return nullptr;
        const std::filesystem::path path = m_Directory / std::filesystem::path(name.C_Str());
        if (context.threadPool)
            return context.textureCache->LoadTextureFromFileAsync(path, sRGB, *context.threadPool);
        return context.textureCache->LoadTextureFromFileDeferred(path, sRGB);
    };

    m_MaterialsByIndex.resize(scene->mNumMaterials, nullptr);
    for (unsigned int i = 0; i < scene->mNumMaterials; ++i)
    {
        const aiMaterial* source = scene->mMaterials[i];
        auto material = std::make_shared<SceneMaterial>();
        m_ImportedMaterials.push_back(material);
        m_MaterialsByIndex[i] = material.get();

        aiString str;
        if (aiGetMaterialString(source, AI_MATKEY_NAME, &str) == AI_SUCCESS)
            material->name = str.C_Str();

        if (aiGetMaterialTexture(source, aiTextureType_DIFFUSE, 0, &str) == AI_SUCCESS)
            material->diffuseTexture = loadTexture(str, true);
        if (aiGetMaterialTexture(source, aiTextureType_SPECULAR, 0, &str) == AI_SUCCESS)
            material->specularTexture = loadTexture(str, true);
        if (aiGetMaterialTexture(source, aiTextureType_HEIGHT, 0, &str) == AI_SUCCESS)
            material->normalsTexture = loadTexture(str, false);
        else if (aiGetMaterialTexture(source, aiTextureType_NORMALS, 0, &str) == AI_SUCCESS)
            material->normalsTexture = loadTexture(str, false);
        if (aiGetMaterialTexture(source, aiTextureType_EMISSIVE, 0, &str) == AI_SUCCESS)
            material->emissiveTexture = loadTexture(str, true);

        aiColor4D color(0.f, 0.f, 0.f, 0.f);
        if (aiGetMaterialColor(source, AI_MATKEY_COLOR_DIFFUSE, &color) == AI_SUCCESS)
            material->diffuseColor = float3(color.r, color.g, color.b);
        if (material->diffuseTexture && all(material->diffuseColor == float3(0.f)))
            material->diffuseColor = float3(1.f);

        color = aiColor4D(0.f, 0.f, 0.f, 0.f);
        if (aiGetMaterialColor(source, AI_MATKEY_COLOR_SPECULAR, &color) == AI_SUCCESS)
            material->specularColor = float3(color.r, color.g, color.b);
        if (material->specularTexture && all(material->specularColor == float3(0.f)))
            material->specularColor = float3(1.f);

        // The raw assimp shininess is used (not normalised); 0.9 when absent.
        float shininess = 0.f;
        material->shininess = (aiGetMaterialFloatArray(source, AI_MATKEY_SHININESS, &shininess, nullptr) == AI_SUCCESS) ? shininess : 0.9f;

        color = aiColor4D(0.f, 0.f, 0.f, 0.f);
        if (aiGetMaterialColor(source, AI_MATKEY_COLOR_EMISSIVE, &color) == AI_SUCCESS)
            material->emissiveColor = float3(color.r, color.g, color.b);
    }
}

void SpaceObject::ResolveMaterials(const aiScene* scene)
{
    // Asteroids.exe: 0x1400521E0
    m_MaterialsByIndex.resize(scene->mNumMaterials, nullptr);
    auto defaultMaterial = m_MaterialsByName.find("default_material");

    for (unsigned int i = 0; i < scene->mNumMaterials; ++i)
    {
        aiString name;
        if (aiGetMaterialString(scene->mMaterials[i], AI_MATKEY_NAME, &name) != AI_SUCCESS)
        {
            log::warning("Material %d doesn't have a name", int(i));
            continue;
        }

        auto it = m_MaterialsByName.find(name.C_Str());
        if (it == m_MaterialsByName.end())
        {
            log::warning("Couldn't find a json entry for material %s", name.C_Str());
            if (defaultMaterial != m_MaterialsByName.end())
                m_MaterialsByIndex[i] = defaultMaterial->second.get();
        }
        else
            m_MaterialsByIndex[i] = it->second.get();
    }
}

void SpaceObject::ResolveMaterials(const chunk2018::MeshSet& meshSet)
{
    // Asteroids.exe: 0x140051F20 (no default_material fallback on this path)
    m_MaterialsByIndex.resize(meshSet.materials.size(), nullptr);
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
            log::warning("Couldn't find a json entry for material %s", name);
        else
            m_MaterialsByIndex[i] = it->second.get();
    }
}

void SpaceObject::CreateMeshesFromAssimp(const aiScene* scene)
{
    // Asteroids.exe: vfunc03 0x140054760
    m_Meshes.resize(scene->mNumMeshes, nullptr);
    uint32_t totalIndices = 0;
    uint32_t totalVertices = 0;

    for (unsigned int i = 0; i < scene->mNumMeshes; ++i)
    {
        const aiMesh* source = scene->mMeshes[i];
        SceneMesh* mesh = new SceneMesh();
        m_Meshes[i] = mesh;

        box3 bounds = box3::empty();
        for (unsigned int v = 0; v < source->mNumVertices; ++v)
            bounds |= float3(source->mVertices[v].x, source->mVertices[v].y, source->mVertices[v].z);

        mesh->objectSpaceBounds = bounds;
        mesh->indexOffset = totalIndices;
        mesh->vertexOffset = totalVertices;
        mesh->buffers = &m_Buffers;
        mesh->numIndices = source->mNumFaces * 3;
        mesh->numVertices = source->mNumVertices;
        mesh->material = source->mMaterialIndex < m_MaterialsByIndex.size() ? m_MaterialsByIndex[source->mMaterialIndex] : nullptr;
        totalIndices += mesh->numIndices;
        totalVertices += mesh->numVertices;
    }

    m_Indices.resize(totalIndices);
    if (m_EnabledAttributes & VertexAttr_Position) m_Positions.resize(totalVertices);
    if (m_EnabledAttributes & VertexAttr_Texcoord1) m_Texcoord1.resize(totalVertices);
    if (m_EnabledAttributes & VertexAttr_Texcoord2) m_Texcoord2.resize(totalVertices);
    if (m_EnabledAttributes & VertexAttr_Normal) m_Normals.resize(totalVertices);
    if (m_EnabledAttributes & VertexAttr_Tangent) m_Tangents.resize(totalVertices);
    if (m_EnabledAttributes & VertexAttr_Bitangent) m_Bitangents.resize(totalVertices);

    for (unsigned int i = 0; i < scene->mNumMeshes; ++i)
    {
        const aiMesh* source = scene->mMeshes[i];
        const SceneMesh* mesh = m_Meshes[i];

        // Mesh-local indices (the vertex offset is applied by the meshlet builder / draw call).
        for (unsigned int f = 0; f < source->mNumFaces; ++f)
        {
            const aiFace& face = source->mFaces[f];
            for (unsigned int k = 0; k < 3; ++k)
                m_Indices[mesh->indexOffset + f * 3 + k] = k < face.mNumIndices ? face.mIndices[k] : 0;
        }

        if ((m_EnabledAttributes & VertexAttr_Position) && source->mNumVertices)
            memcpy(&m_Positions[mesh->vertexOffset], source->mVertices, sizeof(float3) * source->mNumVertices);

        for (unsigned int v = 0; v < source->mNumVertices; ++v)
        {
            const uint32_t dst = mesh->vertexOffset + v;
            if ((m_EnabledAttributes & VertexAttr_Texcoord1) && source->mTextureCoords[0])
                m_Texcoord1[dst] = float2(source->mTextureCoords[0][v].x, source->mTextureCoords[0][v].y);
            if ((m_EnabledAttributes & VertexAttr_Texcoord2) && source->mTextureCoords[1])
                m_Texcoord2[dst] = float2(source->mTextureCoords[1][v].x, source->mTextureCoords[1][v].y);
            if ((m_EnabledAttributes & VertexAttr_Normal) && source->mNormals)
                m_Normals[dst] = PackSnorm8(source->mNormals[v]);
            if ((m_EnabledAttributes & VertexAttr_Tangent) && source->mTangents)
                m_Tangents[dst] = PackSnorm8(source->mTangents[v]);
            if ((m_EnabledAttributes & VertexAttr_Bitangent) && source->mBitangents)
                m_Bitangents[dst] = PackSnorm8(-source->mBitangents[v]);    // negated in 2018
        }
    }
}

void SpaceObject::ProcessNode(const aiScene* scene, const aiNode* node, const std::shared_ptr<SceneNode>& sceneNode,
    const affine3& parentWorld, uint32_t depth, bool instanceAtMeshlessNodes)
{
    // Asteroids.exe: 0x140057C20
    const affine3 local = ConvertTransform(node->mTransformation);
    sceneNode->SetBaseTransform(local);
    const affine3 world = local * parentWorld;

    std::stringstream ss;
    for (uint32_t d = 0; d < depth; ++d)
        ss << "  ";
    ss << node->mName.C_Str() << "/" << node->mNumMeshes << "/" << node->mNumChildren << std::endl;
    OutputDebugStringA(ss.str().c_str());

    SceneMeshInstance* lastInstance = nullptr;
    if (instanceAtMeshlessNodes)
    {
        if (node->mNumMeshes == 0)
        {
            for (unsigned int k = 0; k < scene->mNumMeshes; ++k)
            {
                auto* instance = new SceneMeshInstance();
                instance->transform = instance->previousTransform = world;
                instance->mesh = m_Meshes[k];
                m_MeshInstances.push_back(instance);
                lastInstance = instance;
            }
        }
    }
    else
    {
        // One instance per mesh and per fscene "instances" entry; uses the node's LOCAL transform
        // (the world transform is applied later by SceneNode::UpdateTransforms).
        for (unsigned int i = 0; i < node->mNumMeshes; ++i)
        {
            for (const affine3& placement : m_InstanceTransforms)
            {
                auto* instance = new SceneMeshInstance();
                instance->transform = instance->previousTransform = local * placement;
                instance->mesh = m_Meshes[node->mMeshes[i]];
                instance->name = node->mName.C_Str();
                m_MeshInstances.push_back(instance);
                lastInstance = instance;
            }
        }
    }
    sceneNode->SetMeshInstance(lastInstance);

    for (unsigned int c = 0; c < node->mNumChildren; ++c)
    {
        const aiNode* childNode = node->mChildren[c];
        auto child = std::make_shared<SceneNode>(sceneNode, childNode->mName.C_Str(), nullptr);
        sceneNode->AddChild(child);
        ProcessNode(scene, childNode, child, world, depth + 1, instanceAtMeshlessNodes);
    }
}

bool SpaceObject::LoadChunkFile(const std::filesystem::path& file, bool hasMaterialsJson)
{
    // Asteroids.exe: 0x140055730
    nvrhi::AutoPtr<nvrhi::IDataBlob> blob;
    if (NVRHI_FAILED(m_FS->readFile(file, &blob)) || !blob)
    {
        log::error("Couldn't read file `%s`", file.generic_string().c_str());
        return false;
    }

    m_ChunkMeshSet = chunk2018::LoadMeshSet(blob, file.generic_string().c_str());
    if (!m_ChunkMeshSet)
    {
        log::error("Unable to load chunk file : `%s`", file.generic_string().c_str());
        return false;
    }
    const chunk2018::MeshSet& ms = *m_ChunkMeshSet;

    // The following checks only report (the 2018 code carries on).
    if (ms.meshletMaxVerts != nvmesh2018::c_MeshletMaxVerts || ms.meshletMaxPrims != nvmesh2018::c_MeshletMaxPrims)
        log::error("Chunk file has wrong maxVerts/maxPrims meshlets : `%s`", file.generic_string().c_str());
    if (hasMaterialsJson)
        ResolveMaterials(ms);
    else
        log::error("Materials not supported in chunk files yet");
    if (ms.type != chunk2018::MeshSetType::Meshlet)
        log::error("Chunk file is not meshlets type : `%s`", file.generic_string().c_str());

    auto copyStream = [&ms](auto& dst, const chunk2018::StreamView& view, uint32_t attribute, uint32_t enabled)
    {
        using T = typename std::decay_t<decltype(dst)>::value_type;
        if ((attribute & enabled) && view.data)
        {
            const T* src = static_cast<const T*>(view.data);
            dst.assign(src, src + ms.nverts);
        }
    };
    copyStream(m_Positions, ms.positions, VertexAttr_Position, m_EnabledAttributes);
    copyStream(m_Texcoord1, ms.texcoords0, VertexAttr_Texcoord1, m_EnabledAttributes);
    copyStream(m_Texcoord2, ms.texcoords1, VertexAttr_Texcoord2, m_EnabledAttributes);
    copyStream(m_Normals, ms.normals, VertexAttr_Normal, m_EnabledAttributes);
    copyStream(m_Tangents, ms.tangents, VertexAttr_Tangent, m_EnabledAttributes);
    copyStream(m_Bitangents, ms.bitangents, VertexAttr_Bitangent, m_EnabledAttributes);
    m_Indices.clear();

    m_Meshes.resize(ms.meshletInfos.size(), nullptr);
    for (size_t i = 0; i < ms.meshletInfos.size(); ++i)
    {
        const chunk2018::MeshletInfo& info = ms.meshletInfos[i];
        SceneMesh* mesh = new SceneMesh();
        mesh->objectSpaceBounds = info.bbox;
        mesh->buffers = &m_Buffers;
        mesh->material = info.materialId < m_MaterialsByIndex.size() ? m_MaterialsByIndex[info.materialId] : nullptr;
        m_Meshes[i] = mesh;
    }

    m_MeshInstances.reserve(ms.instances.size());
    for (const chunk2018::MeshInstance& source : ms.instances)
    {
        if (source.minfoId >= ms.meshletInfos.size())
            continue;
        auto* instance = new SceneMeshInstance();
        instance->transform = instance->previousTransform = source.transform;
        instance->transformedBounds = source.bbox;
        instance->mesh = m_Meshes[source.minfoId];
        if (const char* name = ms.string(source.name))
            instance->name = name;
        instance->firstMeshlet = ms.meshletInfos[source.minfoId].firstMeshlet;
        instance->numMeshlets = ms.meshletInfos[source.minfoId].numMeshlets;
        m_MeshInstances.push_back(instance);
    }

    m_MeshletData = std::make_unique<SceneMeshletData>();
    SceneMeshletData& md = *m_MeshletData;
    if (const auto* indices32 = static_cast<const uint32_t*>(ms.indices32.data))
        md.vertexIndices.assign(indices32, indices32 + ms.indices32.elemCount);
    if (const auto* indices8 = static_cast<const uint8_t*>(ms.indices8.data))
        md.primIndices.assign(indices8, indices8 + ms.indices8.elemCount);
    if (const auto* headers = ms.meshletHeaders())
        md.meshlets.assign(headers, headers + ms.meshletCount());
    md.numVertices = ms.nverts;
    md.numPrims = uint32_t(ms.indices8.elemCount / 3);
    for (const chunk2018::MeshletInfo& info : ms.meshletInfos)
        md.subsets.push_back(uint2(info.firstMeshlet, info.numMeshlets));
    md.bounds = ms.bbox;

    m_NumMeshlets = m_NumSubElementMeshlets = ms.meshletCount();
    m_MeshletBounds = ms.bbox;
    m_SceneBounds = ms.bbox;
    return true;
}

void SpaceObject::CreateRenderResources(nvrhi::IDevice* device, nvrhi::ICommandList* commandList,
    engine::CommonRenderPasses& commonPasses, nvrhi::IBindingLayout* materialBindingLayout, bool meshlets, bool skipGeometry)
{
    // Asteroids.exe: 0x140052AD0
    if (meshlets)
    {
        if (m_ChunkMeshSet)
        {
            if (!skipGeometry)
                CreateChunkMeshletBuffers(device, commandList);
        }
        else
            BuildMeshlets(device, commandList, skipGeometry);
        SortInstances();
    }
    else if (!skipGeometry)
    {
        CreateGeometryBuffers(device, commandList);
        SortInstances();
        CreateTransformsBuffer(device, commandList);
    }

    CreateMaterialResources(device, commandList, commonPasses, materialBindingLayout);
}

void SpaceObject::SortInstances()
{
    // Group instances by material, then by mesh (raw pointer order, as in 2018).
    std::sort(m_MeshInstances.begin(), m_MeshInstances.end(), [](const SceneMeshInstance* a, const SceneMeshInstance* b)
    {
        if (a->mesh->material != b->mesh->material)
            return a->mesh->material < b->mesh->material;
        return a->mesh < b->mesh;
    });
}

void SpaceObject::CreateGeometryBuffers(nvrhi::IDevice* device, nvrhi::ICommandList* commandList)
{
    // Asteroids.exe: 0x140052D80
    if (!m_Indices.empty())
    {
        nvrhi::BufferDesc desc;
        desc.byteSize = m_Indices.size() * sizeof(uint32_t);
        desc.isIndexBuffer = true;
        desc.initialState = nvrhi::ResourceStates::IndexBuffer;
        desc.keepInitialState = true;
        device->createBuffer(desc, &m_Buffers.indexBuffer);
        commandList->writeBuffer(m_Buffers.indexBuffer, m_Indices.data(), desc.byteSize);
        std::vector<uint32_t>().swap(m_Indices);
    }

    CreateVertexBuffer(device, commandList, VertexAttr_Position, m_EnabledAttributes, m_Buffers, m_Positions, "AttrPosition");
    CreateVertexBuffer(device, commandList, VertexAttr_Texcoord1, m_EnabledAttributes, m_Buffers, m_Texcoord1, "AttrTexcoord1");
    CreateVertexBuffer(device, commandList, VertexAttr_Texcoord2, m_EnabledAttributes, m_Buffers, m_Texcoord2, "AttrTexcoord2");
    CreateVertexBuffer(device, commandList, VertexAttr_Normal, m_EnabledAttributes, m_Buffers, m_Normals, "AttrNormal");
    CreateVertexBuffer(device, commandList, VertexAttr_Tangent, m_EnabledAttributes, m_Buffers, m_Tangents, "AttrTangent");
    CreateVertexBuffer(device, commandList, VertexAttr_Bitangent, m_EnabledAttributes, m_Buffers, m_Bitangents, "AttrBitangent");
}

void SpaceObject::CreateTransformsBuffer(nvrhi::IDevice* device, nvrhi::ICommandList* commandList)
{
    // Asteroids.exe: 0x140052C00
    if (m_MeshInstances.empty())
        return;

    nvrhi::BufferDesc desc;
    desc.byteSize = 96 * m_MeshInstances.size();
    desc.debugName = "Transforms";
    desc.isVertexBuffer = true;
    desc.initialState = nvrhi::ResourceStates::VertexBuffer;
    desc.keepInitialState = true;
    device->createBuffer(desc, &m_TransformsBuffer);
    m_Buffers.vertexBuffers[VertexAttr_Transform] = m_TransformsBuffer;

    UpdateTransformsBuffer(commandList);
}

void SpaceObject::UpdateTransformsBuffer(nvrhi::ICommandList* commandList)
{
    // Asteroids.exe: 0x1400585D0 — [2i] current, [2i+1] previous; each a column-major float3x4.
    if (!m_TransformsBuffer)
        return;

    std::vector<float> data(m_MeshInstances.size() * 24);
    for (size_t i = 0; i < m_MeshInstances.size(); ++i)
    {
        SceneMeshInstance* instance = m_MeshInstances[i];
        instance->instanceIndex = uint32_t(i);
        affineToColumnMajor(instance->transform, &data[i * 24]);
        affineToColumnMajor(instance->previousTransform, &data[i * 24 + 12]);
    }
    commandList->writeBuffer(m_TransformsBuffer, data.data(), data.size() * sizeof(float));
}

void SpaceObject::BuildMeshlets(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, bool skipBuffers)
{
    // Asteroids.exe: 0x140053A00 — one subset per instance (instances of the same mesh get their own meshlets).
    std::vector<MeshletSubset> subsets;
    subsets.reserve(m_MeshInstances.size());
    for (const SceneMeshInstance* instance : m_MeshInstances)
        subsets.push_back({ instance->mesh->indexOffset, instance->mesh->numIndices / 3, instance->mesh->vertexOffset });

    MeshletBuildDesc desc;
    desc.positions = reinterpret_cast<const uint8_t*>(m_Positions.data());
    desc.positionStride = sizeof(float3);
    desc.numVertices = uint32_t(m_Positions.size());
    desc.indices = m_Indices.data();
    desc.numIndices = uint32_t(m_Indices.size());
    desc.subsets = subsets.data();
    desc.numSubsets = uint32_t(subsets.size());
    desc.rebaseSubsets = true;

    MeshletBuildOptions options;    // 0x00816440: 64 verts, 100 prims, vertex-cache optimisation, cache size 64
    options.maxVerts = nvmesh2018::c_MeshletMaxVerts;
    options.maxPrims = nvmesh2018::c_MeshletMaxPrims;
    options.optimizeVertexCache = true;
    options.cacheSize = 64;

    m_MeshletData = ::BuildMeshlets(desc, options);
    if (!m_MeshletData)
        return;

    m_NumMeshlets = uint32_t(m_MeshletData->meshlets.size());
    m_MeshletBounds = m_MeshletData->bounds;

    if (!skipBuffers)
        CreateMeshletBuffers(device, commandList);

    const auto& outSubsets = m_MeshletData->subsets;
    for (size_t i = 0; i < outSubsets.size() && i < m_MeshInstances.size(); ++i)
    {
        m_MeshInstances[i]->firstMeshlet = outSubsets[i].x;
        m_MeshInstances[i]->numMeshlets = outSubsets[i].y;
    }
    if (!outSubsets.empty())
        m_NumSubElementMeshlets = outSubsets.back().x + outSubsets.back().y;
}

void SpaceObject::CreateMeshletBuffers(nvrhi::IDevice* device, nvrhi::ICommandList* commandList)
{
    // Asteroids.exe: 0x140058930
    if (!m_MeshletData)
    {
        CreateGeometryBuffers(device, commandList);
        return;
    }
    const SceneMeshletData& md = *m_MeshletData;

    // deviation: the 2018 assimp path named the u32 index buffer "Vertex Positions" (copy-paste) and the
    // u8 one "TriangleIndices"; the .chk path names are used for both.
    m_VertexIndicesBuffer = CreateStaticBuffer(device, commandList, md.vertexIndices.data(), md.vertexIndices.size(), "Vertex Positions Indices (32)");
    m_TriangleIndicesBuffer = CreateStaticBuffer(device, commandList, md.primIndices.data(), md.primIndices.size(), "Triangle Indices (8)", 0);
    m_MeshletInfoBuffer = CreateStaticBuffer(device, commandList, md.meshlets.data(), md.meshlets.size(), "Meshlet Info");
    m_SubElementsBuffer = CreateStaticBuffer(device, commandList, md.subsets.data(), md.subsets.size(), "SubElements Info");
    // unresolved: the 2018 object also uploads a 40-byte-per-entry "LOD Info" table when its LOD vector
    // (SpaceObject+440) is non-empty; nothing in the binary fills it, so no buffer is created.

    // A missing stream falls back to the position data, like the 2018 code.
    const uint32_t nv = md.numVertices;
    auto source = [this](const void* data) -> const void* { return data ? data : m_Positions.data(); };
    const void* positions = m_Positions.empty() ? nullptr : m_Positions.data();
    if (!positions)
        return;
    m_PositionsBuffer = CreateStaticBuffer(device, commandList, static_cast<const float3*>(positions), nv, "Vertex Positions");
    m_Texcoord1Buffer = CreateStaticBuffer(device, commandList, static_cast<const float2*>(source(m_Texcoord1.empty() ? nullptr : m_Texcoord1.data())), nv, "Vertex Texcoord1");
    m_Texcoord2Buffer = CreateStaticBuffer(device, commandList, static_cast<const float2*>(source(m_Texcoord2.empty() ? nullptr : m_Texcoord2.data())), nv, "Vertex Texcoord2");
    m_NormalsBuffer = CreateStaticBuffer(device, commandList, static_cast<const uint32_t*>(source(m_Normals.empty() ? nullptr : m_Normals.data())), nv, "Vertex Normals");
    m_TangentsBuffer = CreateStaticBuffer(device, commandList, static_cast<const uint32_t*>(source(m_Tangents.empty() ? nullptr : m_Tangents.data())), nv, "Vertex Tangents");
    m_BitangentsBuffer = CreateStaticBuffer(device, commandList, static_cast<const uint32_t*>(source(m_Bitangents.empty() ? nullptr : m_Bitangents.data())), nv, "Vertex Bitangents");
}

void SpaceObject::CreateChunkMeshletBuffers(nvrhi::IDevice* device, nvrhi::ICommandList* commandList)
{
    // Asteroids.exe: 0x1400532D0
    const chunk2018::MeshSet& ms = *m_ChunkMeshSet;
    const uint32_t nv = ms.nverts;

    m_PositionsBuffer = CreateStaticBuffer(device, commandList, static_cast<const float3*>(ms.positions.data), nv, "Vertex Positions");
    m_Texcoord1Buffer = CreateStaticBuffer(device, commandList, static_cast<const float2*>(ms.texcoords0.data), nv, "Vertex Texcoord1");
    m_Texcoord2Buffer = CreateStaticBuffer(device, commandList, static_cast<const float2*>(ms.texcoords1.data), nv, "Vertex Texcoord2");
    m_NormalsBuffer = CreateStaticBuffer(device, commandList, static_cast<const uint32_t*>(ms.normals.data), nv, "Vertex Normals");
    m_TangentsBuffer = CreateStaticBuffer(device, commandList, static_cast<const uint32_t*>(ms.tangents.data), nv, "Vertex Tangents");
    m_BitangentsBuffer = CreateStaticBuffer(device, commandList, static_cast<const uint32_t*>(ms.bitangents.data), nv, "Vertex Bitangents");
    m_VertexIndicesBuffer = CreateStaticBuffer(device, commandList, static_cast<const uint32_t*>(ms.indices32.data), ms.indices32.elemCount, "Vertex Positions Indices (32)");
    m_TriangleIndicesBuffer = CreateStaticBuffer(device, commandList, static_cast<const uint8_t*>(ms.indices8.data), ms.indices8.elemCount, "Triangle Indices (8)", 0);
    m_MeshletInfoBuffer = CreateStaticBuffer(device, commandList, ms.meshletHeaders(), ms.meshletCount(), "Meshlet Info");

    // The 2018 code first uploads MeshletData::subsets and then replaces the buffer with the same
    // {firstMeshlet, numMeshlets} pairs rebuilt from the meshlet infos; only the second upload is kept.
    std::vector<uint2> subElements;
    subElements.reserve(ms.meshletInfos.size());
    for (const chunk2018::MeshletInfo& info : ms.meshletInfos)
        subElements.push_back(uint2(info.firstMeshlet, info.numMeshlets));
    m_SubElementsBuffer = CreateStaticBuffer(device, commandList, subElements.data(), subElements.size(), "SubElements Info");
}

void SpaceObject::CreateMaterialResources(nvrhi::IDevice* device, nvrhi::ICommandList* commandList,
    engine::CommonRenderPasses& commonPasses, nvrhi::IBindingLayout* materialBindingLayout)
{
    // Asteroids.exe: 0x1400541B0. deviation: null entries are skipped and shared entries
    // (default_material) are only initialized once.
    std::set<SceneMaterial*> done;
    for (SceneMaterial* material : m_MaterialsByIndex)
    {
        if (!material || !done.insert(material).second)
            continue;
        material->CreateResources(device, commandList, commonPasses, materialBindingLayout,
            (m_LoadFlags & LoadFlag_ForceSpecularType3) != 0);
    }
}

void SpaceObject::UpdateBounds()
{
    // Asteroids.exe: 0x140051CC0
    m_SceneBounds = box3::empty();
    for (SceneMeshInstance* instance : m_MeshInstances)
    {
        instance->transformedBounds = TransformBox(instance->mesh->objectSpaceBounds, instance->transform);
        instance->transformedCenter = instance->transformedBounds.m_mins
            + (instance->transformedBounds.m_maxs - instance->transformedBounds.m_mins) * 0.5f;
        m_SceneBounds |= instance->transformedBounds;
    }
}

void SpaceObject::UpdateMaterials(nvrhi::ICommandList* commandList)
{
    // Asteroids.exe: 0x140058530
    for (SceneMaterial* material : m_MaterialsByIndex)
    {
        if (material)
            material->UpdateConstants(commandList);
    }
}

nvrhi::IBuffer* SpaceObject::GetMeshletBuffer(SpaceObjectBufferType type) const
{
    // Asteroids.exe: 0x140054510
    switch (type)
    {
    case SpaceObjectBufferType::VertexPositions: return m_PositionsBuffer;
    case SpaceObjectBufferType::VertexNormals: return m_NormalsBuffer;
    case SpaceObjectBufferType::VertexTexcoord1: return m_Texcoord1Buffer;
    case SpaceObjectBufferType::VertexTexcoord2: return m_Texcoord2Buffer;
    case SpaceObjectBufferType::VertexTangents: return m_TangentsBuffer;
    case SpaceObjectBufferType::VertexBitangents: return m_BitangentsBuffer;
    case SpaceObjectBufferType::VertexIndices: return m_VertexIndicesBuffer;
    case SpaceObjectBufferType::TriangleIndices: return m_TriangleIndicesBuffer;
    case SpaceObjectBufferType::MeshletInfo: return m_MeshletInfoBuffer;
    case SpaceObjectBufferType::SubElementsInfo: return m_SubElementsBuffer;
    case SpaceObjectBufferType::LodInfo: return m_LodInfoBuffer;
    default: return nullptr;
    }
}

void SpaceObject::ClearCpuData()
{
    // Asteroids.exe: 0x140053ED0
    m_MeshletData.reset();
    std::vector<uint32_t>().swap(m_Indices);
    std::vector<float3>().swap(m_Positions);
    std::vector<float2>().swap(m_Texcoord1);
    std::vector<float2>().swap(m_Texcoord2);
    std::vector<uint32_t>().swap(m_Normals);
    std::vector<uint32_t>().swap(m_Tangents);
    std::vector<uint32_t>().swap(m_Bitangents);
}

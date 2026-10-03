/***************************************************************************
 # Copyright (c) 2021-2023, NVIDIA CORPORATION.  All rights reserved.
 #
 # NVIDIA CORPORATION and its licensors retain all intellectual property
 # and proprietary rights in and to this software, related documentation
 # and any modifications thereto.  Any use, reproduction, disclosure or
 # distribution of this software and related documentation without an express
 # license agreement from NVIDIA CORPORATION is strictly prohibited.
 **************************************************************************/

#include "SampleScene.h"
#include <donut/core/json.h>
#include <donut/core/vfs/VFS.h>
#include <nvrhi/utils.h>
#include <nvrhi/common/misc.h>
#include <donut/engine/TextureCache.h>
#include <donut/core/math/math.h>
#include "Types.h"

using namespace donut;
using namespace donut::math;

bool SampleScene::LoadWithThreadPool(const std::filesystem::path& jsonFileName, engine::ThreadPool* threadPool)
{
    if (!Scene::LoadWithThreadPool(jsonFileName, threadPool))
        return false;

    return true;
}

inline uint64_t AdvanceHeapPtr(uint64_t& heapPtr, const nvrhi::MemoryRequirements& memReq)
{
    heapPtr = nvrhi::align(heapPtr, memReq.alignment);
    uint64_t current = heapPtr;

    heapPtr += memReq.size;

    return current;
}

static
void GetMeshBlasDesc(const engine::MeshInfo& mesh, nvrhi::rt::AccelStructDesc& blasDesc) {
    blasDesc.isTopLevel = false;
    blasDesc.debugName = mesh.name;

    for (const auto& geometry : mesh.geometries) {
        nvrhi::rt::GeometryDesc geometryDesc;
        auto& triangles = geometryDesc.geometryData.triangles;
        triangles.indexBuffer = mesh.buffers->indexBuffer;
        triangles.indexOffset =
            (mesh.indexOffset + geometry->indexOffsetInMesh) * sizeof(uint32_t);
        triangles.indexFormat = nvrhi::Format::R32_UINT;
        triangles.indexCount = geometry->numIndices;
        triangles.vertexBuffer = mesh.buffers->vertexBuffer;
        triangles.vertexOffset =
            (mesh.vertexOffset + geometry->vertexOffsetInMesh) * sizeof(float3) +
            mesh.buffers->getVertexBufferRange(engine::VertexAttribute::Position)
                .byteOffset;
        triangles.vertexFormat = nvrhi::Format::RGB32_FLOAT;
        triangles.vertexStride = sizeof(float3);
        triangles.vertexCount = geometry->numVertices;
        geometryDesc.geometryType = nvrhi::rt::GeometryType::Triangles;
        geometryDesc.flags =
            (geometry->material->domain == engine::MaterialDomain::AlphaTested)
                ? nvrhi::rt::GeometryFlags::None
                : nvrhi::rt::GeometryFlags::Opaque;
        blasDesc.bottomLevelGeometries.push_back(geometryDesc);
    }

    // don't compact acceleration structures that are built per frame
    if (mesh.skinPrototype != nullptr) {
        blasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
    } else {
        blasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace |
                              nvrhi::rt::AccelStructBuildFlags::AllowCompaction;
    }
}

void SampleScene::UpdateLightsBuffer(nvrhi::ICommandList *cmdList) {

    auto lights = GetSceneGraph()->GetLights();
    std::sort(lights.begin(), lights.end(), [](const auto& left, const auto& right) {
        return left->GetLightType() < right->GetLightType();
    });

    std::vector<Graphics::LightConstants> lightConstants(lights.size());
    for (size_t i = 0; i < lights.size(); ++i) {
        lights[i]->FillLightConstants((LightConstants&)lightConstants[i]);
    }

    // Create light structured buffer
    nvrhi::BufferDesc bufDesc;
    bufDesc.byteSize = sizeof(Graphics::LightConstants) * lights.size();
    bufDesc.structStride = sizeof(Graphics::LightConstants);
    bufDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    bufDesc.keepInitialState = true;
    bufDesc.debugName = "Light Buffer";
    m_Device->createBuffer(bufDesc, &m_LightConstantsBuffer);

    cmdList->writeBuffer(m_LightConstantsBuffer, lightConstants.data(),
                         sizeof(Graphics::LightConstants) * lightConstants.size());

    m_NumDirectionalLights = 0;
    m_NumSpotLights = 0;
    m_NumSpotLights = 0;

    auto it = std::upper_bound(
        lightConstants.begin(), lightConstants.end(), (uint32_t)LightType_Directional,
        [](uint32_t value, const auto &elem) { return value < elem.lightType; });
    m_NumDirectionalLights = std::distance(lightConstants.begin(), it);
    it = std::upper_bound(it, lightConstants.end(), (uint32_t)LightType_Spot,  [](uint32_t value, const auto& elem) {
        return value < elem.lightType;
    });
    m_NumSpotLights = std::distance(lightConstants.begin(), it) - m_NumDirectionalLights;
    it = std::upper_bound(it, lightConstants.end(), (uint32_t)LightType_Point, [](uint32_t value, const auto& elem) {
        return value < elem.lightType;
    });
    m_NumSpotLights = std::distance(lightConstants.begin(), it) - m_NumSpotLights - m_NumDirectionalLights;
}

void SampleScene::BuildMeshBLASes(nvrhi::ICommandList* cmdList) {
    for (const auto& mesh : GetSceneGraph()->GetMeshes()) {
        if (mesh->isSkinPrototype) continue;  // skip the skinning prototypes

        nvrhi::rt::AccelStructDesc blasDesc;

        GetMeshBlasDesc(*mesh, blasDesc);

        nvrhi::rt::AccelStructHandle as;
        m_Device->createAccelStruct(blasDesc, &as);

        if (!mesh->skinPrototype)
            nvrhi::utils::BuildBottomLevelAccelStruct(cmdList, as, blasDesc);

        mesh->accelStruct = as;
    }

    nvrhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.isTopLevel = true;
    tlasDesc.topLevelMaxInstances = GetSceneGraph()->GetMeshInstances().size();
    cmdList->getDevice()->createAccelStruct(tlasDesc, &m_TopLevelAS);
}

void SampleScene::BuildTLAS(nvrhi::ICommandList* commandList, uint32_t frameIndex)
{
    commandList->beginMarker("Skinned BLAS Updates");

    // Transition all the buffers to their necessary states before building the BLAS'es to
    // allow BLAS batching
    for (const auto& skinnedInstance :
         GetSceneGraph()->GetSkinnedMeshInstances()) {
        if (skinnedInstance->GetLastUpdateFrameIndex() < frameIndex) continue;

        commandList->setAccelStructState(skinnedInstance->GetMesh()->accelStruct,
                                         nvrhi::ResourceStates::AccelStructWrite);
        commandList->setBufferState(skinnedInstance->GetMesh()->buffers->vertexBuffer,
                                    nvrhi::ResourceStates::AccelStructBuildInput);
    }
    commandList->commitBarriers();

    // Now build the BLAS'es
    for (const auto& skinnedInstance :
         GetSceneGraph()->GetSkinnedMeshInstances()) {
        if (skinnedInstance->GetLastUpdateFrameIndex() < frameIndex) continue;

        nvrhi::rt::AccelStructDesc blasDesc;
        GetMeshBlasDesc(*skinnedInstance->GetMesh(), blasDesc);

        nvrhi::utils::BuildBottomLevelAccelStruct(
            commandList, skinnedInstance->GetMesh()->accelStruct, blasDesc);
    }
    commandList->endMarker();

    std::vector<nvrhi::rt::InstanceDesc> instances;

    for (const auto& instance : GetSceneGraph()->GetMeshInstances()) {
        nvrhi::rt::InstanceDesc instanceDesc;
        instanceDesc.bottomLevelAS = instance->GetMesh()->accelStruct;
        assert(instanceDesc.bottomLevelAS);
        instanceDesc.instanceMask = 1;
        instanceDesc.instanceID = instance->GetInstanceIndex();

        auto node = instance->GetNode();
        assert(node);
        dm::affineToColumnMajor(node->GetLocalToWorldTransformFloat(),
                                instanceDesc.transform);

        instances.push_back(instanceDesc);
    }

    // Compact acceleration structures that are tagged for compaction and have finished
    // executing the original build
    commandList->compactBottomLevelAccelStructs();

    commandList->beginMarker("TLAS Update");
    commandList->buildTopLevelAccelStruct(m_TopLevelAS, instances.data(), instances.size());
    commandList->endMarker();
}

void SampleScene::Animate(float fElapsedTimeSeconds)
{
    m_WallclockTime += fElapsedTimeSeconds;
    float offset = 0;

    for (const auto& anim : GetSceneGraph()->GetAnimations()) {
        float duration = anim->GetDuration();
        float integral;
        float animationTime =
            std::modf((m_WallclockTime + offset) / duration, &integral) * duration;
        (void)anim->Apply(animationTime);
        offset += 1.0f;
    }
}

nvrhi::rt::IAccelStruct* SampleScene::GetTopLevelAS() const
{
    return m_TopLevelAS;
}

nvrhi::IBuffer* SampleScene::GetLightConstantsBuffer() const {
    return m_LightConstantsBuffer;
}

uint32_t SampleScene::GetNumDirectionalLights() const { return m_NumDirectionalLights; }

uint32_t SampleScene::GetNumSpotLights() const { return m_NumSpotLights; }

uint32_t SampleScene::GetNumPointLights() const { return m_NumSpotLights; }


/***************************************************************************
 # Copyright (c) 2021-2023, NVIDIA CORPORATION.  All rights reserved.
 #
 # NVIDIA CORPORATION and its licensors retain all intellectual property
 # and proprietary rights in and to this software, related documentation
 # and any modifications thereto.  Any use, reproduction, disclosure or
 # distribution of this software and related documentation without an express
 # license agreement from NVIDIA CORPORATION is strictly prohibited.
 **************************************************************************/

#pragma once

#include <donut/engine/Scene.h>
#include <donut/engine/KeyframeAnimation.h>

class SampleScene : public donut::engine::Scene
{
public:
    using Scene::Scene;

    bool LoadWithThreadPool(const std::filesystem::path& sceneFileName,
                            donut::engine::ThreadPool *threadPool);

    const donut::engine::SceneGraphAnimation* GetBenchmarkAnimation() const;
    const donut::engine::PerspectiveCamera* GetBenchmarkCamera() const;

    void UpdateLightsBuffer(nvrhi::ICommandList *cmdList);

    void BuildMeshBLASes(nvrhi::ICommandList *cmdList);
    void BuildTLAS(nvrhi::ICommandList* commandList, uint32_t frameIndex);
    void Animate(float  fElapsedTimeSeconds);

    nvrhi::rt::IAccelStruct* GetTopLevelAS() const;
    nvrhi::IBuffer *GetLightConstantsBuffer() const;
    uint32_t GetNumDirectionalLights() const;
    uint32_t GetNumSpotLights() const;
    uint32_t GetNumPointLights() const;

 private:
    nvrhi::rt::AccelStructHandle m_TopLevelAS;
    nvrhi::BufferHandle m_LightConstantsBuffer;
    uint32_t m_NumDirectionalLights;
    uint32_t m_NumSpotLights;
    uint32_t m_NumPointLights;

    double m_WallclockTime = 0;
};

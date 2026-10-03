/*
 * Copyright (c) 2019-2023, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#ifndef DESCRIPTORS_HLSL
#define DESCRIPTORS_HLSL

#pragma pack_matrix(row_major)

#include "../Types.h"
#include "Platform.hlsli"
#include <donut/shaders/view_cb.h>
#include <donut/shaders/material_cb.h>
#include <donut/shaders/bindless.h>

// Global Root / Push Constants
// ------------------------------------------------------------------------------------
VK_PUSH_CONSTANT ConstantBuffer<GlobalRootConstants> GlobalRootConst: register(b0);

ConstantBuffer<FrameConstants> FrameConst: register(b1);
ConstantBuffer<GlobalConstants> GlobalConst : register(b2);

//----------------------------------------------------------------------------------------------------------------
// Root Signature Descriptors and Mappings
// ---------------------------------------------------------------------------------------------------------------

// Samplers
// -------------------------------------------------------------------------------------------------
SamplerState Samplers[3]: register(s0);

// Structured Buffers
// ---------------------------------------------------------------------------------------

StructuredBuffer<LightConstants> Lights : register(t0);
StructuredBuffer<MaterialConstants> Materials : register(t1);
StructuredBuffer<InstanceData> TLASInstances : register(t2);
StructuredBuffer<GeometryData> BLASGeometries : register(t3);
RaytracingAccelerationStructure SceneBVH : register(t4);
StructuredBuffer<DDGIVolumeDescGPUPacked> DDGIVolumes: register(t5);
StructuredBuffer<DDGIVolumeResourceIndices> DDGIVolumeBindless: register(t6);
// Scene Bindless Resources
VK_BINDING(0, 1) ByteAddressBuffer SceneBuffers[] : register(t0, space1);
VK_BINDING(1, 1) Texture2D SceneTex2D[] : register(t0, space2);

// Fixed Bindless Resources
// ---------------------------------------------------------------------------------------
VK_BINDING(0, 2) Texture2D<float4> FixedTex2D[]: register(t0, space3);
VK_BINDING(0, 2) Texture2DArray<float4> FixedTex2DArray[]: register(t0, space4);
VK_BINDING(1, 2) RWTexture2D<float4> FixedRWTex2D[]: register(u0, space1);
VK_BINDING(1, 2) RWTexture2DArray<float4> FixedRWTex2DArray[]: register(u0, space2);

// Defines for Convenience
// ----------------------------------------------------------------------------------
#define BLUE_NOISE_SRV_INDEX 0
#define PT_OUTPUT_UAV_INDEX 1
#define PT_ACCUMULATION_UAV_INDEX 2
#define GBUFFERA_SRV_INDEX 3
#define GBUFFERA_UAV_INDEX 4
#define GBUFFERB_SRV_INDEX 5
#define GBUFFERB_UAV_INDEX 6
#define GBUFFERC_SRV_INDEX 7
#define GBUFFERC_UAV_INDEX 8
#define GBUFFERD_SRV_INDEX 9
#define GBUFFERD_UAV_INDEX 10
#define DDGI_OUTPUT_SRV_INDEX 11
#define DDGI_OUTPUT_UAV_INDEX 12
#define RTAO_OUTPUT_SRV_INDEX 13
#define RTAO_OUTPUT_UAV_INDEX 14
#define RTAO_RAW_SRV_INDEX 15
#define RTAO_RAW_UAV_INDEX 16

#define GetGlobalRootConst(x, y) (GlobalRootConst.x##_##y)
#define GetGlobalConst(x, y) (GlobalConst.x##_##y)

uint GetFrameNumber() { return FrameConst.frameNumber; }

uint GetPTNumBounces() { return (GetGlobalConst(pt, numBounces) & 0x7FFFFFFF); }
uint GetPTProgressive() { return (GetGlobalConst(pt, numBounces) & 0x80000000); }

uint GetPTSamplesPerPixel() { return (GetGlobalConst(pt, samplesPerPixel) & 0x3FFFFFFF); }
uint GetPTAntialiasing() { return (GetGlobalConst(pt, samplesPerPixel) & 0x80000000); }
uint GetPTShaderExecutionReordering() {
    return GetGlobalConst(pt, samplesPerPixel) & 0x40000000;
}

uint HasDirectionalLight() { return GetGlobalConst(lighting, hasDirectionalLight); }
uint GetNumPointLights() { return GetGlobalConst(lighting, numPointLights); }
uint GetNumSpotLights() { return GetGlobalConst(lighting, numSpotLights); }

// Sampler Accessor Functions
// ------------------------------------------------------------------------------

SamplerState GetBilinearWrapSampler() { return Samplers[0]; }
SamplerState GetPointClampSampler() { return Samplers[1]; }
SamplerState GetAnisoWrapSampler() { return Samplers[2]; }

// Resource Accessor Functions
// ------------------------------------------------------------------------------

PlanarViewConstants GetCamera() {
    return FrameConst.view;
}

float3 GetCameraRight() { return FrameConst.view.matViewToWorld[0].xyz; }
float3 GetCameraUp() { return FrameConst.view.matViewToWorld[1].xyz; }
float3 GetCameraFwd() { return FrameConst.view.matViewToWorld[2].xyz; }
float3 GetCameraPos() { return FrameConst.view.matViewToWorld[3].xyz; }

StructuredBuffer<LightConstants> GetLights() { return Lights; }

void GetGeometryData(uint instanceID, uint geometryIndex, out GeometryData geometry) {
    InstanceData instance = TLASInstances[instanceID];
    geometry = BLASGeometries[instance.firstGeometryIndex + geometryIndex];
}
MaterialConstants GetMaterial(GeometryData geometry) {
    return Materials[geometry.materialIndex];
}

RaytracingAccelerationStructure GetSceneBVH() { return SceneBVH; }

ByteAddressBuffer GetSceneBuffer(uint index) { return SceneBuffers[NonUniformResourceIndex(index)]; }
Texture2D<float4> GetSceneTex2D(uint index) { return SceneTex2D[NonUniformResourceIndex(index)]; }
Texture2D<float4> GetFixedTex2DSRV(uint index) { return FixedTex2D[NonUniformResourceIndex(index + GetGlobalConst(fixedRes, fixedResourceIndexOffset))]; }
RWTexture2D<float4> GetFixedTex2DUAV(uint index) { return FixedRWTex2D[NonUniformResourceIndex(index + GetGlobalConst(fixedRes, fixedResourceIndexOffset))]; }

#define GetFixedTex2D(name, type) GetFixedTex2D##type(name##_##type##_INDEX)

uint GetNumVolumes() { return GetGlobalConst(fixedRes, numDDGIVolumes); }

#endif  // DESCRIPTORS_HLSL

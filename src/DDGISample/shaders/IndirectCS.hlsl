/*
* Copyright (c) 2019-2023, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

// -------------------------------------------------------------------------------------------

#include "ddgi/DDGICommon.hlsli"
#include "ddgi/DDGIApplicationBridge.hlsli"
#include "ddgi/Irradiance.hlsli"
#include "Common.hlsli"
#include "Descriptors.hlsli"

#define THGP_DIM_X 8
#define THGP_DIM_Y 4

// ---[ Compute Shader ]---

[numthreads(THGP_DIM_X, THGP_DIM_Y, 1)]
void CS(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    float3 color = float3(0.f, 0.f, 0.f);

    // Get the (bindless) resources
    Texture2D<float4> GBufferA = GetFixedTex2D(GBUFFERA, SRV);
    Texture2D<float4> GBufferB = GetFixedTex2D(GBUFFERB, SRV);
    Texture2D<float4> GBufferC = GetFixedTex2D(GBUFFERC, SRV);
    RWTexture2D<float4> DDGIOutput = GetFixedTex2D(DDGI_OUTPUT, UAV);

    // Load the albedo and primary ray hit distance
    float4 albedo = GBufferA.Load(int3(DispatchThreadID.xy, 0));

    // Primary ray hit, need to light it
    if (albedo.a > 0.f)
    {
        // Convert albedo back to linear
        albedo.rgb = SRGBToLinear(albedo.rgb);

        // Load the world position, hit distance, and normal
        float4 worldPosHitT = GBufferB.Load(int3(DispatchThreadID.xy, 0));
        float3 normal = GBufferC.Load(int3(DispatchThreadID.xy, 0)).xyz;

        // Compute indirect lighting
        float3 irradiance = 0.f;

        // TODO: sort volumes by density, screen-space area, and/or other prioritization heuristics
        for(uint volumeIndex = 0; volumeIndex < GetNumVolumes(); volumeIndex++)
        {
            DDGIVolumeDescGPU volume = GetDDGIVolumeDescGPU(volumeIndex);
            DDGIVolumeResourceIndices resourceIndices = GetDDGIResourceIndices(volumeIndex);

            float3 cameraDirection = normalize(worldPosHitT.xyz - GetCameraPos());
            float3 surfaceBias = DDGIGetSurfaceBias(normal, cameraDirection, volume);

            // Get the volume's resources
            DDGIVolumeResources resources;
            resources.probeIrradiance = GetProbeIrradianceSRV(resourceIndices);
            resources.probeDistance = GetProbeDistanceSRV(resourceIndices);
            resources.probeData = GetProbeDataSRV(resourceIndices);
            resources.bilinearSampler = GetBilinearWrapSampler();

            // Get the blend weight for this volume's contribution to the surface
            float blendWeight = DDGIGetVolumeBlendWeight(worldPosHitT.xyz, volume);
            if(blendWeight > 0)
            {
                // Get irradiance for the world-space position in the volume
                irradiance += DDGIGetVolumeIrradiance(
                    worldPosHitT.xyz,
                    surfaceBias,
                    normal,
                    volume,
                    resources);

                irradiance *= blendWeight;
            }
        }

        // Compute final color
        color = (albedo.rgb / PI) * irradiance;
    }

    DDGIOutput[DispatchThreadID.xy] = float4(color, 1.f);
}

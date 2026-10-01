/*
* Copyright (c) 2019-2023, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

// For example usage, see DDGI_[D3D12|VK].cpp::CompileDDGIVolumeShaders() function.

#include "DDGICommon.hlsli"
#include "DDGIApplicationBridge.hlsli"

[numthreads(32, 1, 1)]
void DDGIProbeClassificationCS(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    // Get the volume's index
    uint volumeIndex = GetDDGIVolumeIndex();

    // Compute the probe index for this thread
    uint probeIndex = DispatchThreadID.x;

    DDGIVolumeDescGPU volume = GetDDGIVolumeDescGPU();
    DDGIVolumeResourceIndices resourceIndices = GetDDGIResourceIndices();

    // Early out: if this thread maps past the number of probes in the volume
    int numProbes = (volume.probeCounts.x * volume.probeCounts.y * volume.probeCounts.z);
    if (probeIndex >= numProbes) return;

    RWTexture2DArray<float4> RayData = GetRayDataUAV(resourceIndices);
    RWTexture2DArray<float4> ProbeData = GetProbeDataUAV(resourceIndices);

    // Get the number of ray samples to inspect
    int numRays = min(volume.probeNumRays, RTXGI_DDGI_NUM_FIXED_RAYS);

    int rayIndex;
    int backfaceCount = 0;
    float hitDistances[RTXGI_DDGI_NUM_FIXED_RAYS];

    // Load the hit distances and count the number of backface hits
    for (rayIndex = 0; rayIndex < RTXGI_DDGI_NUM_FIXED_RAYS; rayIndex++)
    {
        // Get the coordinates for the probe ray in the RayData texture array
        int3 rayDataTexCoords = DDGIGetRayDataTexelCoords(rayIndex, probeIndex, volume);

        // Load the hit distance for the ray
        hitDistances[rayIndex] = DDGILoadProbeRayDistance(RayData, rayDataTexCoords, volume);

        // Increment the count if a backface is hit
        backfaceCount += (hitDistances[rayIndex] < 0.f);
    }

    // Get the probe's texel coordinates in the Probe Data texture array
    uint3 outputCoords = DDGIGetProbeTexelCoords(probeIndex, volume);

    // Early out: number of backface hits has been exceeded. The probe is probably inside geometry.
    if(((float)backfaceCount / (float)RTXGI_DDGI_NUM_FIXED_RAYS) > volume.probeFixedRayBackfaceThreshold)
    {
        ProbeData[outputCoords].w = RTXGI_DDGI_PROBE_STATE_INACTIVE;
        return;
    }

    // Get the world space position of the probe
    int3 probeCoords = DDGIGetProbeCoords(probeIndex, volume);
    float3 probeWorldPosition = DDGIGetProbeWorldPosition(probeCoords, volume, ProbeData);

    // Determine if there is nearby geometry in the probe's voxel.
    // Iterate over the probe rays and compare ray hit distances with
    // the distance(s) to the probe's voxel planes.
    for (rayIndex = 0; rayIndex < RTXGI_DDGI_NUM_FIXED_RAYS; rayIndex++)
    {
        // Skip backface hits
        if(hitDistances[rayIndex] < 0) continue;

        // Get the direction of the "fixed" ray
        float3 direction = DDGIGetProbeRayDirection(rayIndex, volume);

        // Get the plane normals
        float3 xNormal = float3(direction.x / max(abs(direction.x), 0.000001f), 0.f, 0.f);
        float3 yNormal = float3(0.f, direction.y / max(abs(direction.y), 0.000001f), 0.f);
        float3 zNormal = float3(0.f, 0.f, direction.z / max(abs(direction.z), 0.000001f));

        // Get the relevant planes to intersect
        float3 p0x = probeWorldPosition + (volume.probeSpacing.x * xNormal);
        float3 p0y = probeWorldPosition + (volume.probeSpacing.y * yNormal);
        float3 p0z = probeWorldPosition + (volume.probeSpacing.z * zNormal);

        // Get the ray's intersection distance with each plane
        float3 distances = 
        {
            dot((p0x - probeWorldPosition), xNormal) / max(dot(direction, xNormal), 0.000001f),
            dot((p0y - probeWorldPosition), yNormal) / max(dot(direction, yNormal), 0.000001f),
            dot((p0z - probeWorldPosition), zNormal) / max(dot(direction, zNormal), 0.000001f)
        };

        // If the ray is parallel to the plane, it will never intersect
        // Set the distance to a very large number for those planes
        if (distances.x == 0.f) distances.x = 1e27f;
        if (distances.y == 0.f) distances.y = 1e27f;
        if (distances.z == 0.f) distances.z = 1e27f;

        // Get the distance to the closest plane intersection
        float maxDistance = min(distances.x, min(distances.y, distances.z));

        // If the hit distance is less than the closest plane intersection, the probe should be active
        if(hitDistances[rayIndex] <= maxDistance)
        {
            ProbeData[outputCoords].w = RTXGI_DDGI_PROBE_STATE_ACTIVE;
            return;
        }
    }

    ProbeData[outputCoords].w = RTXGI_DDGI_PROBE_STATE_INACTIVE;
}


[numthreads(32, 1, 1)]
void DDGIProbeClassificationResetCS(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    // Get the volume's index
    uint volumeIndex = GetDDGIVolumeIndex();
    DDGIVolumeDescGPU volume = GetDDGIVolumeDescGPU();
    DDGIVolumeResourceIndices resourceIndices = GetDDGIResourceIndices();
    RWTexture2DArray<float4> ProbeData = GetProbeDataUAV(resourceIndices);

    // Get the probe's texel coordinates in the Probe Data texture
    uint3 outputCoords = DDGIGetProbeTexelCoords(DispatchThreadID.x, volume);

    // Set all probes to active
    ProbeData[outputCoords].w = RTXGI_DDGI_PROBE_STATE_ACTIVE;
}

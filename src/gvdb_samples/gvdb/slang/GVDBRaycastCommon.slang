#ifndef GVDB_RAYCAST_COMMON_SLANG
#define GVDB_RAYCAST_COMMON_SLANG
// Sparse-volume ray marching on graphics and compute backends.
// SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct RaycastParameters {
    VolumeInfo volume;
    uint4 image;           // width, height, transfer-function entries, maximum steps
    float4 rayOrigin;
    float4 direction00;    // xyz: lower image corner; w: 0 ray distance, 1 view-Z clipping
    float4 directionDx;    // per-pixel direction increments, including perspective
    float4 directionDy;
    float4 march;          // step size, minimum t, maximum t, extinction multiplier
    float4 valueRange;     // transfer minimum/maximum, isovalue, termination opacity
    float4 background;
    float4 light;          // index-space light position, ambient fraction
};
ConstantBuffer<RaycastParameters> g_Parameters : register(b0);
#if defined(GVDB_ATLAS_RAW)
ByteAddressBuffer g_Atlas : register(t0);
#else
StructuredBuffer<float> g_Atlas : register(t0);
#endif
ByteAddressBuffer g_Nodes : register(t1);
ByteAddressBuffer g_Children : register(t2);
StructuredBuffer<LevelInfo> g_Levels : register(t3);
StructuredBuffer<float4> g_Transfer : register(t4);
RWStructuredBuffer<float4> g_Output : register(u0);
RWStructuredBuffer<float> g_Depth : register(u1);
#include "GVDBRayClip.slang"

// Explicit zero-direction handling avoids 0*infinity NaNs for rays on a face.
bool intersectBounds(float3 origin,float3 direction,float minimumT,float maximumT,out float enter,out float leave) {
    enter=minimumT;
    leave=maximumT;
    if (all(direction == 0) || !all(isfinite(direction))) return false;
    for (uint axis = 0; axis < 3; ++axis) {
        float lower = g_Parameters.volume.boundsMin[axis];
        float upper = g_Parameters.volume.boundsMax[axis];
        if (direction[axis] == 0) {
            if (origin[axis] < lower || origin[axis] >= upper) return false;
        } else {
            float a = (lower - origin[axis]) / direction[axis];
            float b = (upper - origin[axis]) / direction[axis];
            enter = max(enter, min(a,b));
            leave = min(leave, max(a,b));
        }
    }
    return enter < leave;
}
bool intersectVolume(float3 origin,float3 direction,out float enter,out float leave) {
    float2 clip=rayClipInterval(direction);
    return intersectBounds(origin,direction,clip.x,clip.y,enter,leave);
}
float atlasValue(int3 voxel) {
    uint3 resolution = g_Parameters.volume.atlasResolutionAndApron.xyz;
    uint3 clamped = uint3(clamp(voxel, int3(0), int3(resolution) - 1));
#if defined(GVDB_ATLAS_RAW)
    return asfloat(g_Atlas.Load(flatten3(clamped, resolution) * 4u));
#else
    return g_Atlas[flatten3(clamped, resolution)];
#endif
}
float sampleVolume(float3 position) {
    VDBNode leaf;
    uint leafIndex;
    if (!findLeaf(g_Nodes, g_Children, g_Levels, g_Parameters.volume, position, leaf, leafIndex)) return 0;
    float3 coordinate = float3(leaf.atlasPosition) + position - float3(leaf.position) - 0.5;
    int3 lower = int3(floor(coordinate));
    float3 weight = coordinate - float3(lower);
    float c00 = lerp(atlasValue(lower), atlasValue(lower + int3(1,0,0)), weight.x);
    float c10 = lerp(atlasValue(lower + int3(0,1,0)), atlasValue(lower + int3(1,1,0)), weight.x);
    float c01 = lerp(atlasValue(lower + int3(0,0,1)), atlasValue(lower + int3(1,0,1)), weight.x);
    float c11 = lerp(atlasValue(lower + int3(0,1,1)), atlasValue(lower + int3(1,1,1)), weight.x);
    return lerp(lerp(c00,c10,weight.y), lerp(c01,c11,weight.y),weight.z);
}
float4 transferValue(float density) {
    float coordinate = saturate((density - g_Parameters.valueRange.x) / (g_Parameters.valueRange.y - g_Parameters.valueRange.x)) * float(g_Parameters.image.z - 1);
    uint lower = uint(coordinate);
    return lerp(g_Transfer[lower], g_Transfer[min(lower + 1, g_Parameters.image.z - 1)], coordinate - float(lower));
}
float3 pixelDirection(uint2 pixel) {
    float3 direction = g_Parameters.direction00.xyz + (float(pixel.x) + 0.5) * g_Parameters.directionDx.xyz +
        (float(pixel.y) + 0.5) * g_Parameters.directionDy.xyz;
    float lengthSquared = dot(direction, direction);
    return lengthSquared > 0 && isfinite(lengthSquared) ? direction * rsqrt(lengthSquared) : float3(0);
}

#endif

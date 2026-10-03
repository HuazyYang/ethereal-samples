/*
* Copyright (c) 2019-2023, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#ifndef RTXGI_COMMON_HLSL
#define RTXGI_COMMON_HLSL
#include "../../ddgi/DDGITypes.h"

//------------------------------------------------------------------------
// Defines
//------------------------------------------------------------------------

// Bindless resource implementation type
#define RTXGI_BINDLESS_TYPE_RESOURCE_ARRAYS 0
#define RTXGI_BINDLESS_TYPE_DESCRIPTOR_HEAP 1

// Texture formats (matches EDDGIVolumeTextureFormat)
#define RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_U32 0
#define RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_F16 1
#define RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_F16x2 2
#define RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_F16x4 3
#define RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_F32 4
#define RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_F32x2 5
#define RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_F32x4 6

// The number of fixed rays that are used by probe relocation and classification.
// These rays directions are always the same to produce temporally stable results.
#define RTXGI_DDGI_NUM_FIXED_RAYS 32

// Probe classification states
#define RTXGI_DDGI_PROBE_STATE_ACTIVE 0     // probe shoots rays and may be sampled by a front facing surface or another probe (recursive irradiance)
#define RTXGI_DDGI_PROBE_STATE_INACTIVE 1   // probe doesn't need to shoot rays, it isn't near a front facing surface

// Volume movement types
#define RTXGI_DDGI_VOLUME_MOVEMENT_TYPE_DEFAULT 0
#define RTXGI_DDGI_VOLUME_MOVEMENT_TYPE_SCROLLING 1

#define RTXGI_DDGI_BLEND_SCROLL_SHARED_MEMORY 1

#define RTXGI_DDGI_DEBUG_PROBE_INDEXING 0

#define RTXGI_DDGI_DEBUG_OCTAHEDRAL_INDEXING 0

#define RTXGI_DDGI_DEBUG_BORDER_COPY_INDEXING 0

//------------------------------------------------------------------------
// Configurations
//------------------------------------------------------------------------
#ifndef RTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS
#define RTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS 6
#endif

#ifndef RTXGI_DDGI_PROBE_NUM_TEXELS
#define RTXGI_DDGI_PROBE_NUM_TEXELS 8
#endif

#ifndef RTXGI_DDGI_BLEND_RADIANCE
#define RTXGI_DDGI_BLEND_RADIANCE 1
#endif

#ifndef RTXGI_DDGI_BLEND_SHARED_MEMORY
#define RTXGI_DDGI_BLEND_SHARED_MEMORY 1
#endif

#ifndef RTXGI_DDGI_BLEND_RAYS_PER_PROBE
#define RTXGI_DDGI_BLEND_RAYS_PER_PROBE 256
#endif

#ifndef RTXGI_DDGI_WAVE_LANE_COUNT
#define RTXGI_DDGI_WAVE_LANE_COUNT 32
#endif

// Coordinate system defines
#define RTXGI_COORDINATE_SYSTEM_LEFT 0
#define RTXGI_COORDINATE_SYSTEM_LEFT_Z_UP 1
#define RTXGI_COORDINATE_SYSTEM_RIGHT 2
#define RTXGI_COORDINATE_SYSTEM_RIGHT_Z_UP 3

// Define RTXGI_COORDINATE_SYSTEM before including DDGIVolume.h and before compiling
// SDK HLSL shaders to use another coordinate system. Default is right handed, y-up.
// Exposed in CMake as RTXGI_COORDINATE_SYSTEM
#define RTXGI_COORDINATE_SYSTEM 2

//------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------

bool IsVolumeMovementScrolling(DDGIVolumeDescGPU volume)
{
    return (volume.movementType == RTXGI_DDGI_VOLUME_MOVEMENT_TYPE_SCROLLING);
}

static const float RTXGI_PI = 3.1415926535897932f;
static const float RTXGI_2PI = 6.2831853071795864f;

//------------------------------------------------------------------------
// Math Helpers
//------------------------------------------------------------------------

/**
 * Returns the largest component of the vector.
 */
float RTXGIMaxComponent(float3 a)
{
    return max(a.x, max(a.y, a.z));
}

/**
 * Returns either -1 or 1 based on the sign of the input value.
 * If the input is zero, 1 is returned.
 */
float RTXGISignNotZero(float v)
{
    return (v >= 0.f) ? 1.f : -1.f;
}

/**
 * 2-component version of RTXGISignNotZero.
 */
float2 RTXGISignNotZero(float2 v)
{
    return float2(RTXGISignNotZero(v.x), RTXGISignNotZero(v.y));
}

//------------------------------------------------------------------------
// Sampling Helpers
//------------------------------------------------------------------------

/**
 * Computes a low discrepancy spherically distributed direction on the unit sphere,
 * for the given index in a set of samples. Each direction is unique in
 * the set, but the set of directions is always the same.
 */
float3 RTXGISphericalFibonacci(float sampleIndex, float numSamples)
{
    const float b = (sqrt(5.f) * 0.5f + 0.5f) - 1.f;
    float phi = RTXGI_2PI * frac(sampleIndex * b);
    float cosTheta = 1.f - (2.f * sampleIndex + 1.f) * (1.f / numSamples);
    float sinTheta = sqrt(saturate(1.f - (cosTheta * cosTheta)));

    return float3((cos(phi) * sinTheta), (sin(phi) * sinTheta), cosTheta);
}

//------------------------------------------------------------------------
// Format Conversion Helpers
//------------------------------------------------------------------------

/**
 * Return the given float value as an unsigned integer within the given numerical scale.
 */
uint RTXGIFloatToUint(float v, float scale)
{
    return (uint)floor(v * scale + 0.5f);
}

/**
 * Pack a float3 into a 32-bit unsigned integer.
 * All channels use 10 bits and 2 bits are unused.
 * Compliment of RTXGIUintToFloat3().
 */
uint RTXGIFloat3ToUint(float3 input)
{
    return (RTXGIFloatToUint(input.r, 1023.f)) | (RTXGIFloatToUint(input.g, 1023.f) << 10) | (RTXGIFloatToUint(input.b, 1023.f) << 20);
}

/**
 * Unpack a packed 32-bit unsigned integer to a float3.
 * Compliment of RTXGIFloat3ToUint().
 */
float3 RTXGIUintToFloat3(uint input)
{
    float3 output;
    output.x = (float)(input & 0x000003FF) / 1023.f;
    output.y = (float)((input >> 10) & 0x000003FF) / 1023.f;
    output.z = (float)((input >> 20) & 0x000003FF) / 1023.f;
    return output;
}

//------------------------------------------------------------------------
// Quaternion Helpers
//------------------------------------------------------------------------

/**
 * Rotate vector v with quaternion q.
 */
float3 RTXGIQuaternionRotate(float3 v, float4 q)
{
    float3 b = q.xyz;
    float b2 = dot(b, b);
    return (v * (q.w * q.w - b2) + b * (dot(v, b) * 2.f) + cross(b, v) * (q.w * 2.f));
}

/**
 * Quaternion conjugate.
 * For unit quaternions, conjugate equals inverse.
 * Use this to create a quaternion that rotates in the opposite direction.
 */
float4 RTXGIQuaternionConjugate(float4 q)
{
    return float4(-q.xyz, q.w);
}

//------------------------------------------------------------------------
// Luminance Helper
//------------------------------------------------------------------------

/**
 * Convert Linear RGB value to Luminance
 */
float RTXGILinearRGBToLuminance(float3 rgb)
{
    const float3 LuminanceWeights = float3(0.2126, 0.7152, 0.0722);
    return dot(rgb, LuminanceWeights);
}


//------------------------------------------------------------------------
// Probe Indexing Helpers
//------------------------------------------------------------------------

/**
 * Get the number of probes on a horizontal plane, in the active coordinate system.
 */
int DDGIGetProbesPerPlane(int3 probeCounts)
{
#if RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_LEFT || RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_RIGHT
    return (probeCounts.x * probeCounts.z);
#elif RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_LEFT_Z_UP || RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_RIGHT_Z_UP
    return (probeCounts.x * probeCounts.y);
#endif
}

/**
 * Get the index of the horizontal plane, in the active coordinate system.
 */
int DDGIGetPlaneIndex(int3 probeCoords)
{
#if RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_LEFT_Z_UP || RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_RIGHT_Z_UP
    return probeCoords.z;
#else
    return probeCoords.y;
#endif
}

/**
 * Get the index of a probe within a horizontal plane that the probe coordinates map to, in the active coordinate system.
 */
int DDGIGetProbeIndexInPlane(int3 probeCoords, int3 probeCounts)
{
#if RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_LEFT || RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_RIGHT
    return probeCoords.x + (probeCounts.x * probeCoords.z);
#elif RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_LEFT_Z_UP
    return probeCoords.y + (probeCounts.y * probeCoords.x);
#elif RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_RIGHT_Z_UP
    return probeCoords.x + (probeCounts.x * probeCoords.y);
#endif
}

/**
 * Get the index of a probe within a horizontal plane (i.e. Texture2DArray slice) that the
 * given texel coordinates map to, in the active coordinate system. Provided 2D texel coordinates
 * should *not* include the octahedral texture's 1-texel border.
 */
int DDGIGetProbeIndexInPlane(uint3 texCoords, int3 probeCounts, int probeNumTexels)
{
#if RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_LEFT || RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_RIGHT || RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_RIGHT_Z_UP
    return int(texCoords.x / probeNumTexels) + (probeCounts.x * int(texCoords.y / probeNumTexels));
#elif RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_LEFT_Z_UP
    return int(texCoords.x / probeNumTexels) + (probeCounts.y * int(texCoords.y / probeNumTexels));
#endif
}

//------------------------------------------------------------------------
// Probe Indices
//------------------------------------------------------------------------

/**
 * Computes the probe index from 3D grid coordinates.
 * The opposite of DDGIGetProbeCoords(probeIndex,...).
 */
int DDGIGetProbeIndex(int3 probeCoords, DDGIVolumeDescGPU volume)
{
    int probesPerPlane = DDGIGetProbesPerPlane(volume.probeCounts);
    int planeIndex = DDGIGetPlaneIndex(probeCoords);
    int probeIndexInPlane = DDGIGetProbeIndexInPlane(probeCoords, volume.probeCounts);

    return (planeIndex * probesPerPlane) + probeIndexInPlane;
}

/**
 * Computes the probe index from 3D (Texture2DArray) texture coordinates.
 */
int DDGIGetProbeIndex(uint3 texCoords, int probeNumTexels, DDGIVolumeDescGPU volume)
{
    int probesPerPlane = DDGIGetProbesPerPlane(volume.probeCounts);
    int probeIndexInPlane = DDGIGetProbeIndexInPlane(texCoords, volume.probeCounts, probeNumTexels);

    return (texCoords.z * probesPerPlane) + probeIndexInPlane;
}

//------------------------------------------------------------------------
// Probe Grid Coordinates
//------------------------------------------------------------------------

/**
 * Computes the 3D grid-space coordinates for the probe at the given probe index in the range [0, numProbes-1].
 * The opposite of DDGIGetProbeIndex(probeCoords,...).
 */
int3 DDGIGetProbeCoords(int probeIndex, DDGIVolumeDescGPU volume)
{
    int3 probeCoords;

#if RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_LEFT || RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_RIGHT
    probeCoords.x = probeIndex % volume.probeCounts.x;
    probeCoords.y = probeIndex / (volume.probeCounts.x * volume.probeCounts.z);
    probeCoords.z = (probeIndex / volume.probeCounts.x) % volume.probeCounts.z;
#elif RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_LEFT_Z_UP
    probeCoords.x = (probeIndex / volume.probeCounts.y) % volume.probeCounts.x;
    probeCoords.y = probeIndex % volume.probeCounts.y;
    probeCoords.z = probeIndex / (volume.probeCounts.x * volume.probeCounts.y);
#elif RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_RIGHT_Z_UP
    probeCoords.x = probeIndex % volume.probeCounts.x;
    probeCoords.y = (probeIndex / volume.probeCounts.x) % volume.probeCounts.y;
    probeCoords.z = probeIndex / (volume.probeCounts.y * volume.probeCounts.x);
#endif

    return probeCoords;
}

/**
 * Computes the 3D grid-space coordinates of the "base" probe (i.e. floor of xyz) of the 8-probe
 * cube that surrounds the given world space position. The other seven probes of the cube
 * are offset by 0 or 1 in grid space along each axis.
 *
 * This function accounts for scroll offsets to adjust the volume's origin.
 */
int3 DDGIGetBaseProbeGridCoords(float3 worldPosition, DDGIVolumeDescGPU volume)
{
    // Get the vector from the volume origin to the surface point
    float3 position = worldPosition - (volume.origin + (volume.probeScrollOffsets * volume.probeSpacing));

    // Rotate the world position into the volume's space
    if(!IsVolumeMovementScrolling(volume)) position = RTXGIQuaternionRotate(position, RTXGIQuaternionConjugate(volume.rotation));

    // Shift from [-n/2, n/2] to [0, n] (grid space)
    position += (volume.probeSpacing * (volume.probeCounts - 1)) * 0.5f;

    // Quantize the position to grid space
    int3 probeCoords = int3(position / volume.probeSpacing);

    // Clamp to [0, probeCounts - 1]
    // Snaps positions outside of grid to the grid edge
    probeCoords = clamp(probeCoords, int3(0, 0, 0), (volume.probeCounts - int3(1, 1, 1)));

    return probeCoords;
}

//------------------------------------------------------------------------
// Texture Coordinates
//------------------------------------------------------------------------

/**
 * Computes the RayData Texture2DArray coordinates of the probe at the given probe index.
 *
 * When infinite scrolling is enabled, probeIndex is expected to be the scroll adjusted probe index.
 * Obtain the adjusted index with DDGIGetScrollingProbeIndex().
 */
uint3 DDGIGetRayDataTexelCoords(int rayIndex, int probeIndex, DDGIVolumeDescGPU volume)
{
    int probesPerPlane = DDGIGetProbesPerPlane(volume.probeCounts);

    uint3 coords;
    coords.x = rayIndex;
    coords.z = probeIndex / probesPerPlane;
    coords.y = probeIndex - (coords.z * probesPerPlane);

    return coords;
}

/**
 * Computes the Texture2DArray coordinates of the probe at the given probe index.
 *
 * When infinite scrolling is enabled, probeIndex is expected to be the scroll adjusted probe index.
 * Obtain the adjusted index with DDGIGetScrollingProbeIndex().
 */
uint3 DDGIGetProbeTexelCoords(int probeIndex, DDGIVolumeDescGPU volume)
{
    // Find the probe's plane index
    int probesPerPlane = DDGIGetProbesPerPlane(volume.probeCounts);
    int planeIndex = int(probeIndex / probesPerPlane);

#if RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_LEFT || RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_RIGHT
    int x = (probeIndex % volume.probeCounts.x);
    int y = (probeIndex / volume.probeCounts.x) % volume.probeCounts.z;
#elif RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_LEFT_Z_UP
    int x = (probeIndex % volume.probeCounts.y);
    int y = (probeIndex / volume.probeCounts.y) % volume.probeCounts.x;
#elif RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_RIGHT_Z_UP
    int x = (probeIndex % volume.probeCounts.x);
    int y = (probeIndex / volume.probeCounts.x) % volume.probeCounts.y;
#endif

    return uint3(x, y, planeIndex);
}

/**
 * Computes the normalized texture UVs within the Probe Irradiance and Probe Distance texture arrays
 * given the probe index and 2D normalized octant coordinates [-1, 1]. Used when sampling the texture arrays.
 * 
 * When infinite scrolling is enabled, probeIndex is expected to be the scroll adjusted probe index.
 * Obtain the adjusted index with DDGIGetScrollingProbeIndex().
 */
float3 DDGIGetProbeUV(int probeIndex, float2 octantCoordinates, int numProbeInteriorTexels, DDGIVolumeDescGPU volume)
{
    // Get the probe's texel coordinates, assuming one texel per probe
    uint3 coords = DDGIGetProbeTexelCoords(probeIndex, volume);

    // Add the border texels to get the total texels per probe
    float numProbeTexels = (numProbeInteriorTexels + 2.f);

#if RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_LEFT || RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_RIGHT
    float textureWidth = numProbeTexels * volume.probeCounts.x;
    float textureHeight = numProbeTexels * volume.probeCounts.z;
#elif RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_LEFT_Z_UP
    float textureWidth = numProbeTexels * volume.probeCounts.y;
    float textureHeight = numProbeTexels * volume.probeCounts.x;
#elif RTXGI_COORDINATE_SYSTEM == RTXGI_COORDINATE_SYSTEM_RIGHT_Z_UP
    float textureWidth = numProbeTexels * volume.probeCounts.x;
    float textureHeight = numProbeTexels * volume.probeCounts.y;
#endif

    // Move to the center of the probe and move to the octant texel before normalizing
    float2 uv = float2(coords.x * numProbeTexels, coords.y * numProbeTexels) + (numProbeTexels * 0.5f);
    uv += octantCoordinates.xy * ((float)numProbeInteriorTexels * 0.5f);
    uv /= float2(textureWidth, textureHeight);
    return float3(uv, coords.z);
}

//------------------------------------------------------------------------
// Probe Classification
//------------------------------------------------------------------------

/**
 * Loads and returns the probe's classification state (from a RWTexture2DArray).
 */
float DDGILoadProbeState(int probeIndex, RWTexture2DArray<float4> probeData, DDGIVolumeDescGPU volume)
{
    float state = RTXGI_DDGI_PROBE_STATE_ACTIVE;
    if (volume.probeClassificationEnabled)
    {
        // Get the probe's texel coordinates in the Probe Data texture
        int3 probeDataCoords = DDGIGetProbeTexelCoords(probeIndex, volume);

        // Get the probe's classification state
        state = probeData[probeDataCoords].w;
    }

    return state;
}

/**
 * Loads and returns the probe's classification state (from a Texture2DArray).
 */
float DDGILoadProbeState(int probeIndex, Texture2DArray<float4> probeData, DDGIVolumeDescGPU volume)
{
    float state = RTXGI_DDGI_PROBE_STATE_ACTIVE;
    if (volume.probeClassificationEnabled)
    {
        // Get the probe's texel coordinates in the Probe Data texture
        int3 probeDataCoords = DDGIGetProbeTexelCoords(probeIndex, volume);

        // Get the probe's classification state
        state = probeData.Load(int4(probeDataCoords, 0)).w;
    }

    return state;
}

//------------------------------------------------------------------------
// Infinite Scrolling
//------------------------------------------------------------------------

/**
 * Adjusts the probe index for when infinite scrolling is enabled.
 * This can run when scrolling is disabled since zero offsets result
 * in the same probe index.
 */
int DDGIGetScrollingProbeIndex(int3 probeCoords, DDGIVolumeDescGPU volume)
{
    return DDGIGetProbeIndex(((probeCoords + volume.probeScrollOffsets + volume.probeCounts) % volume.probeCounts), volume);
}

/**
 * Clears probe irradiance and distance data for a plane of probes that have been scrolled to new positions.
 */
bool DDGIClearScrolledPlane(int3 probeCoords, int planeIndex, DDGIVolumeDescGPU volume)
{
    if (volume.probeScrollClear[planeIndex])
    {
        int offset = volume.probeScrollOffsets[planeIndex];
        int probeCount = volume.probeCounts[planeIndex];
        int direction = volume.probeScrollDirections[planeIndex];

        int coord = 0;
        if(direction) coord = (probeCount + (offset - 1)) % probeCount; // scrolling in positive direction
        else coord = (probeCount + (offset % probeCount)) % probeCount; // scrolling in negative direction

        // Probe has scrolled and needs to be cleared
        if (probeCoords[planeIndex] == coord) return true;
    }
    return false;
}


//------------------------------------------------------------------------
// Probe Ray Data Texture Write Helpers
//------------------------------------------------------------------------

void DDGIStoreProbeRayMiss(RWTexture2DArray<float4> RayData, uint3 coords, DDGIVolumeDescGPU volume, float3 radiance)
{
    if (volume.probeRayDataFormat == RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_F32x4)
    {
        RayData[coords] = float4(radiance, 1e27f);
    }
    else if (volume.probeRayDataFormat == RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_F32x2)
    {
        RayData[coords] = float4(asfloat(RTXGIFloat3ToUint(radiance)), 1e27f, 0.f, 0.f);
    }
}

void DDGIStoreProbeRayFrontfaceHit(RWTexture2DArray<float4> RayData, uint3 coords, DDGIVolumeDescGPU volume, float3 radiance, float hitT)
{
    if (volume.probeRayDataFormat == RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_F32x4)
    {
        // Store color components and hit distance as 32-bit float values.
        RayData[coords] = float4(radiance, hitT);
    }
    else if (volume.probeRayDataFormat == RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_F32x2)
    {
        // Use R32G32_FLOAT format (don't use R32G32_UINT since hit distance needs to be negative sometimes).
        // Pack color as R10G10B10 in R32 and store hit distance in G32.
        static const float c_threshold = 1.f / 255.f;
        if (RTXGIMaxComponent(radiance.rgb) <= c_threshold) radiance.rgb = float3(0.f, 0.f, 0.f);
        RayData[coords] = float4(asfloat(RTXGIFloat3ToUint(radiance.rgb)), hitT, 0.f, 0.f);
    }
}

void DDGIStoreProbeRayFrontfaceHit(RWTexture2DArray<float4> RayData, uint3 coords, DDGIVolumeDescGPU volume, float hitT)
{
    if (volume.probeRayDataFormat == RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_F32x4)
    {
        RayData[coords].w = hitT;
    }
    else if (volume.probeRayDataFormat == RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_F32x2)
    {
        RayData[coords].g = hitT;
    }
}

void DDGIStoreProbeRayBackfaceHit(RWTexture2DArray<float4> RayData, uint3 coords, DDGIVolumeDescGPU volume, float hitT)
{
    // Make the hit distance negative to mark a backface hit for blending, probe relocation, and probe classification.
    // Shorten the hit distance on a backface hit by 80% to decrease the influence of the probe during irradiance sampling.
    if (volume.probeRayDataFormat == RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_F32x4)
    {
        RayData[coords].w = -hitT * 0.2f;
    }
    else if (volume.probeRayDataFormat == RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_F32x2)
    {
        RayData[coords].g = -hitT * 0.2f;
    }
}

//------------------------------------------------------------------------
// Probe Ray Data Texture Read Helpers
//------------------------------------------------------------------------

float3 DDGILoadProbeRayRadiance(RWTexture2DArray<float4> RayData, uint3 coords, DDGIVolumeDescGPU volume)
{
    if (volume.probeRayDataFormat == RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_F32x4)
    {
        return RayData[coords].rgb;
    }
    else if (volume.probeRayDataFormat == RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_F32x2)
    {
        return RTXGIUintToFloat3(asuint(RayData[coords].r));
    }
    return float3(0.f, 0.f, 0.f);
}

float DDGILoadProbeRayDistance(RWTexture2DArray<float4> RayData, uint3 coords, DDGIVolumeDescGPU volume)
{
    if (volume.probeRayDataFormat == RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_F32x4)
    {
        return RayData[coords].a;
    }
    else if (volume.probeRayDataFormat == RTXGI_DDGI_VOLUME_TEXTURE_FORMAT_F32x2)
    {
        return RayData[coords].g;
    }
    return 0.f;
}

//------------------------------------------------------------------------
// Probe Ray Direction
//------------------------------------------------------------------------

/**
 * Computes a spherically distributed, normalized ray direction for the given ray index in a set of ray samples.
 * Applies the volume's random probe ray rotation transformation to "non-fixed" ray direction samples.
 */
float3 DDGIGetProbeRayDirection(int rayIndex, DDGIVolumeDescGPU volume)
{
    bool isFixedRay = false;
    int sampleIndex = rayIndex;
    int numRays = volume.probeNumRays;

    if (volume.probeRelocationEnabled || volume.probeClassificationEnabled)
    {
        isFixedRay = (rayIndex < RTXGI_DDGI_NUM_FIXED_RAYS);
        sampleIndex = isFixedRay ? rayIndex : (rayIndex - RTXGI_DDGI_NUM_FIXED_RAYS);
        numRays = isFixedRay ? RTXGI_DDGI_NUM_FIXED_RAYS : (numRays - RTXGI_DDGI_NUM_FIXED_RAYS);
    }

    // Get a ray direction on the sphere
    float3 direction = RTXGISphericalFibonacci(sampleIndex, numRays);

    // Don't rotate fixed rays so relocation/classification are temporally stable
    if (isFixedRay) return normalize(direction);

    // Apply a random rotation and normalize the direction
    return normalize(RTXGIQuaternionRotate(direction, RTXGIQuaternionConjugate(volume.probeRayRotation)));
}


//------------------------------------------------------------------------
// Probe Octahedral Indexing
//------------------------------------------------------------------------

/**
 * Computes normalized octahedral coordinates for the given texel coordinates.
 * Maps the top left texel to (-1,-1).
 * Used by DDGIProbeBlendingCS() in ProbeBlending.hlsl.
 */
float2 DDGIGetNormalizedOctahedralCoordinates(int2 texCoords, int numTexels)
{
    // Map 2D texture coordinates to a normalized octahedral space
    float2 octahedralTexelCoord = float2(texCoords.x % numTexels, texCoords.y % numTexels);

    // Move to the center of a texel
    octahedralTexelCoord.xy += 0.5f;

    // Normalize
    octahedralTexelCoord.xy /= float(numTexels);

    // Shift to [-1, 1);
    octahedralTexelCoord *= 2.f;
    octahedralTexelCoord -= float2(1.f, 1.f);

    return octahedralTexelCoord;
}

/**
 * Computes the normalized octahedral direction that corresponds to the
 * given normalized coordinates on the [-1, 1] square.
 * The opposite of DDGIGetOctahedralCoordinates().
 * Used by DDGIProbeBlendingCS() in ProbeBlending.hlsl.
 */
float3 DDGIGetOctahedralDirection(float2 coords)
{
    float3 direction = float3(coords.x, coords.y, 1.f - abs(coords.x) - abs(coords.y));
    if (direction.z < 0.f)
    {
        direction.xy = (1.f - abs(direction.yx)) * RTXGISignNotZero(direction.xy);
    }
    return normalize(direction);
}

/**
 * Computes the octant coordinates in the normalized [-1, 1] square, for the given a unit direction vector.
 * The opposite of DDGIGetOctahedralDirection().
 * Used by GetDDGIVolumeIrradiance() in Irradiance.hlsl.
 */
float2 DDGIGetOctahedralCoordinates(float3 direction)
{
    float l1norm = abs(direction.x) + abs(direction.y) + abs(direction.z);
    float2 uv = direction.xy * (1.f / l1norm);
    if (direction.z < 0.f)
    {
        uv = (1.f - abs(uv.yx)) * RTXGISignNotZero(uv.xy);
    }
    return uv;
}

//------------------------------------------------------------------------
// Probe Data Texture Write Helpers
//------------------------------------------------------------------------

/**
 * Normalizes the world-space offset and writes it to the probe data texture.
 * Probe Relocation limits this range to [0.f, 0.45f).
 */
void DDGIStoreProbeDataOffset(RWTexture2DArray<float4> probeData, uint3 coords, float3 wsOffset, DDGIVolumeDescGPU volume)
{
    probeData[coords].xyz = wsOffset / volume.probeSpacing;
}

//------------------------------------------------------------------------
// Probe Data Texture Read Helpers
//------------------------------------------------------------------------

/**
 * Reads the probe's position offset (from a Texture2DArray) and converts it to a world-space offset.
 */
float3 DDGILoadProbeDataOffset(Texture2DArray<float4> probeData, uint3 coords, DDGIVolumeDescGPU volume)
{
    return probeData.Load(int4(coords, 0)).xyz * volume.probeSpacing;
}

/**
 * Reads the probe's position offset (from a RWTexture2DArray) and converts it to a world-space offset.
 */
float3 DDGILoadProbeDataOffset(RWTexture2DArray<float4> probeData, uint3 coords, DDGIVolumeDescGPU volume)
{
    return probeData[coords].xyz * volume.probeSpacing;
}


//------------------------------------------------------------------------
// Probe World Position
//------------------------------------------------------------------------

/**
 * Computes the world-space position of a probe from the probe's 3D grid-space coordinates.
 * Probe relocation is not considered.
 */
float3 DDGIGetProbeWorldPosition(int3 probeCoords, DDGIVolumeDescGPU volume)
{
    // Multiply the grid coordinates by the probe spacing
    float3 probeGridWorldPosition = probeCoords * volume.probeSpacing;

    // Shift the grid of probes by half of each axis extent to center the volume about its origin
    float3 probeGridShift = (volume.probeSpacing * (volume.probeCounts - 1)) * 0.5f;

    // Center the probe grid about the origin
    float3 probeWorldPosition = (probeGridWorldPosition - probeGridShift);

    // Rotate the probe grid if infinite scrolling is not enabled
    if (!IsVolumeMovementScrolling(volume)) probeWorldPosition = RTXGIQuaternionRotate(probeWorldPosition, volume.rotation);

    // Translate the grid to the volume's center
    probeWorldPosition += volume.origin + (volume.probeScrollOffsets * volume.probeSpacing);

    return probeWorldPosition;
}

/**
 * Computes the world-space position of a probe from the probe's 3D grid-space coordinates.
 * When probe relocation is enabled, offsets are loaded from the probe data
 * Texture2D and used to adjust the final world position.
 */
float3 DDGIGetProbeWorldPosition(int3 probeCoords, DDGIVolumeDescGPU volume, Texture2DArray<float4> probeData)
{
    // Get the probe's world-space position
    float3 probeWorldPosition = DDGIGetProbeWorldPosition(probeCoords, volume);

    // If the volume has probe relocation enabled, account for the probe offsets
    if (volume.probeRelocationEnabled)
    {
        // Get the scroll adjusted probe index
        int probeIndex = DDGIGetScrollingProbeIndex(probeCoords, volume);

        // Find the texture coordinates of the probe in the Probe Data texture
        uint3 coords = DDGIGetProbeTexelCoords(probeIndex, volume);

        // Load the probe's world-space position offset and add it to the current world position
        probeWorldPosition += DDGILoadProbeDataOffset(probeData, coords, volume);
    }

    return probeWorldPosition;
}

/**
 * Computes the world-space position of a probe from the probe's 3D grid-space coordinates.
 * When probe relocation is enabled, offsets are loaded from the probe data
 * RWTexture2D and used to adjust the final world position.
 */
float3 DDGIGetProbeWorldPosition(int3 probeCoords, DDGIVolumeDescGPU volume, RWTexture2DArray<float4> probeData)
{
    // Get the probe's world-space position
    float3 probeWorldPosition = DDGIGetProbeWorldPosition(probeCoords, volume);

    // If the volume has probe relocation enabled, account for the probe offsets
    if (volume.probeRelocationEnabled)
    {
        // Get the scroll adjusted probe index
        int probeIndex = DDGIGetScrollingProbeIndex(probeCoords, volume);

        // Find the texture coordinates of the probe in the Probe Data texture
        uint3 coords = DDGIGetProbeTexelCoords(probeIndex, volume);

        // Load the probe's world-space position offset and add it to the current world position
        probeWorldPosition += DDGILoadProbeDataOffset(probeData, coords, volume);
    }

    return probeWorldPosition;
}

#endif // RTXGI_COMMON_HLSL

/*
 * Resource declarations and helpers shared by the meshlet task (amplification) and mesh shaders.
 *
 * All names and register/space bindings are taken from the DXIL reflection of the 2018 Asteroids
 * shaders. Resources that a particular shader does not reference are removed from its compiled
 * binding table by DXC, exactly like in the original binaries.
 *
 * The original 2018 NVAPI mesh-shader path also declared
 *     RWStructuredBuffer<NvShaderExtnStruct> g_NvidiaExt : register(u7);
 * which no longer exists in this SM 6.5 port.
 */

#ifndef MESHLET_COMMON_HLSLI
#define MESHLET_COMMON_HLSLI

#include "meshlet_cb.h"

// ---------------------------------------------------------------------------------------------
// Constant buffers (space1)
// ---------------------------------------------------------------------------------------------

cbuffer cbFrame        : register(b0, space1) { FrameCB         cbFrame;        };
cbuffer cbMeshletInfo  : register(b1, space1) { MeshletInfoCB   cbMeshletInfo;  };
cbuffer cbInstance     : register(b2, space1) { InstanceCB      cbInstance;     };
cbuffer cbInstancePrev : register(b3, space1) { InstanceCB      cbInstancePrev; };
cbuffer cbObjectInfo   : register(b4, space1) { ObjectConstants cbObjectInfo;   };
cbuffer cbSectorInfo   : register(b5, space1) { SectorInfo      cbSectorInfo;   };

// ---------------------------------------------------------------------------------------------
// Geometry streams (space1). See docs/formats.md, "NVDACHNK chunk files".
// ---------------------------------------------------------------------------------------------

StructuredBuffer<float3>  vertexPositionBuffer  : register(t0, space1);
StructuredBuffer<uint>    vertexNormalBuffer    : register(t1, space1);  // snorm8 x,y,z (w byte unused)
StructuredBuffer<float2>  vertexTexcoord1Buffer : register(t2, space1);
StructuredBuffer<float2>  vertexTexcoord2Buffer : register(t3, space1);  // declared in the original, never referenced; register inferred
StructuredBuffer<uint>    vertexTangentBuffer   : register(t4, space1);  // snorm8 x,y,z
StructuredBuffer<uint>    vertexBitangentBuffer : register(t5, space1);  // snorm8 x,y,z
StructuredBuffer<uint>    indexBuffer           : register(t6, space1);  // meshlet vertex-index list (u32)
ByteAddressBuffer         primBuffer            : register(t7, space1);  // meshlet-local triangle indices, 3 x u8 per triangle
StructuredBuffer<uint4>   meshletBuffer         : register(t8, space1);  // 16-byte meshlet descriptors

// Declaration order below follows the resource IDs recorded in the original binaries.
Texture1D<float>          t_ViewDistance        : register(t16, space1);
Texture2D<float>          t_ZFar                : register(t17, space1);  // Hi-Z pyramid written by create_hi_z_cs

SamplerState              s_ViewDistanceSampler : register(s0, space1);
SamplerState              s_ZFarSampler         : register(s1, space1);

RWStructuredBuffer<uint>  u_Stats               : register(u1, space1);  // [0] = asteroids rejected by the Hi-Z test

StructuredBuffer<LODInfo>          lodInfoBuffer      : register(t10, space1);
StructuredBuffer<AsteroidInstance> instanceInfoBuffer : register(t11, space1);

// ---------------------------------------------------------------------------------------------
// Meshlet descriptor (uint4):
//   x: [31:24] vertexCount, [23:0] 3 x u8 quantized bounding-box minimum (relative to bboxLods[lod])
//   y: [31:24] primCount,   [23:0] 3 x u8 quantized bounding-box maximum
//   z: vertexOffset into indexBuffer
//   w: primOffset, byte offset into primBuffer (3 bytes per triangle)
// ---------------------------------------------------------------------------------------------

uint getMeshletVertexCount(uint4 meshlet)    { return meshlet.x >> 24; }
uint getMeshletPrimitiveCount(uint4 meshlet) { return meshlet.y >> 24; }

uint3 unpackBytes3(uint packed)
{
    return uint3(packed & 0xff, (packed >> 8) & 0xff, (packed >> 16) & 0xff);
}

// snorm8x3 -> float3, decoded by hand: a byte >= 128 is negative (two's complement).
float3 unpackSnorm8x3(uint packed)
{
    uint3 bytes = unpackBytes3(packed);
    int3 values = int3(bytes) - int3((bytes >> 7) << 8);
    return float3(values) / 127.0;
}

// Reads 4 bytes from a byte offset that is not necessarily dword aligned.
uint loadUnalignedDword(ByteAddressBuffer buffer, uint byteOffset)
{
    uint alignedOffset = byteOffset & ~3u;
    uint shift = (byteOffset & 3) * 8;
    uint dword0 = buffer.Load(alignedOffset) >> shift;
    if (shift != 0)
        dword0 |= buffer.Load(alignedOffset + 4) << (32 - shift);
    return dword0;
}

void getMeshletPrimIndices(uint byteOffset, out uint i0, out uint i1, out uint i2)
{
    uint3 indices = unpackBytes3(loadUnalignedDword(primBuffer, byteOffset));
    i0 = indices.x;
    i1 = indices.y;
    i2 = indices.z;
}

// ---------------------------------------------------------------------------------------------
// Culling / LOD helpers used by the asteroid task shaders
// ---------------------------------------------------------------------------------------------

bool IsSphereFrustumCulled(float3 center, float radius)
{
    [unroll]
    for (int i = 0; i < 6; i++)
    {
        float4 plane = cbFrame.frustumPlanes[i];
        if (dot(plane.xyz, center) - plane.w > radius)
            return true;
    }
    return false;
}

// Corner i of an AABB: bit 0 selects max.x, bit 1 max.y, bit 2 max.z.
float4 getBoxCorner(float3 bboxMin, float3 bboxMax, int i)
{
    switch (i)
    {
    case 0: return float4(bboxMin.x, bboxMin.y, bboxMin.z, 1.0);
    case 1: return float4(bboxMax.x, bboxMin.y, bboxMin.z, 1.0);
    case 2: return float4(bboxMin.x, bboxMax.y, bboxMin.z, 1.0);
    case 3: return float4(bboxMax.x, bboxMax.y, bboxMin.z, 1.0);
    case 4: return float4(bboxMin.x, bboxMin.y, bboxMax.z, 1.0);
    case 5: return float4(bboxMax.x, bboxMin.y, bboxMax.z, 1.0);
    case 6: return float4(bboxMin.x, bboxMax.y, bboxMax.z, 1.0);
    case 7: return float4(bboxMax.x, bboxMax.y, bboxMax.z, 1.0);
    }
    return 0;
}

// View-distance fade: t_ViewDistance stores the visible distance as a function of the vertical
// view direction (u = dir.y * 0.5 + 0.5). The sphere is probed at its top and bottom points.
float computeViewDistanceFade(float3 viewVector, float radius)
{
    float3 top = viewVector + float3(0, radius, 0);
    float topDistance = length(top);
    float topFade = saturate((t_ViewDistance.SampleLevel(s_ViewDistanceSampler, top.y / topDistance * 0.5 + 0.5, 0) - topDistance) * 0.001);

    float3 bottom = viewVector - float3(0, radius, 0);
    float bottomDistance = length(bottom);
    float bottomFade = saturate((t_ViewDistance.SampleLevel(s_ViewDistanceSampler, bottom.y / bottomDistance * 0.5 + 0.5, 0) - bottomDistance) * 0.001);

    return max(topFade, bottomFade);
}

float computeLOD2(float screenDiameter, float objectLodBias)
{
    if (screenDiameter < 1.0)
        return 0;

    float lod = (log2(screenDiameter) + objectLodBias) * cbFrame.lodSlope + cbFrame.lodBias;
    return clamp(lod, 0, cbObjectInfo.maxLevelToRender);
}

// Continuous LOD from the projected diameter (in pixels) of the bounding sphere.
float computeLOD(float3 viewVector, float radius)
{
    float screenDiameter = radius * 2.0 * max(cbFrame.matProjectionForLod[0][0] * cbFrame.rtDims.x,
                                              cbFrame.matProjectionForLod[1][1] * cbFrame.rtDims.y);
    if (cbFrame.orthographicProjection == 0)
        screenDiameter /= max(0.01, length(viewVector));

    return computeLOD2(screenDiameter, cbObjectInfo.lodBias);
}

// Projects the object bounding box (cbObjectInfo.bbox) to the screen and compares its nearest
// depth against the farthest depth stored in the Hi-Z pyramid (t_ZFar) over the covered area.
// Returns false (not culled) when any corner is behind the camera or the footprint needs a
// Hi-Z level that does not exist. Used by asteroidTS with _MESHLETS_HI_Z=1.
bool IsOcclusionCulled(float3x4 instanceMat, uint asteroidIndex /* unused in the shipped code */)
{
    float2 rectMin = cbFrame.rtDims;
    float2 rectMax = 0;
    float minDepth = 1.0;

    [unroll]
    for (int i = 0; i < 8; i++)
    {
        float4 corner = getBoxCorner(cbObjectInfo.bbox.bboxMin.xyz, cbObjectInfo.bbox.bboxMax.xyz, i);
        float3 worldPos = mul(instanceMat, corner) + cbSectorInfo.sectorOffset;
        float4 clipPos = mul(float4(worldPos, 1.0), cbFrame.matWorldToClip);

        if (!(clipPos.w > 0))
            return false;

        float3 ndc = clipPos.xyz / clipPos.w;
        float2 screenPos = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * cbFrame.rtDims;

        rectMin = min(rectMin, screenPos);
        rectMax = max(rectMax, screenPos);
        minDepth = min(minDepth, ndc.z);
    }

    rectMin = max(rectMin, 0);
    rectMax = min(rectMax, cbFrame.rtDims);

    float2 rectSize = rectMax - rectMin;
    int mipLevel = int(ceil(log2(max(rectSize.x, rectSize.y) * cbFrame.invZFarTileSize)));

    if (!(float(mipLevel) < cbFrame.zFarNumLevels))
        return false;

    float2 uv = (rectMin + rectMax) * 0.5 * cbFrame.invZFarTileSize * cbFrame.invZFarDims;
    float farDepth = t_ZFar.SampleLevel(s_ZFarSampler, uv, float(mipLevel));

    return minDepth > farDepth;
}

// Culls the meshlet bounding sphere (derived from the quantized meshlet AABB) against the
// frustum planes. Returns true when the meshlet is outside. Used by asteroidMS.
bool earlyCullUsingSphere(uint4 meshlet, float3x4 instanceMat, float uniformScale, float3 sectorOffset,
                          float4 lodBboxMin, float4 lodBboxMax)
{
    float3 extent = lodBboxMax.xyz - lodBboxMin.xyz;
    float3 quantMin = float3(unpackBytes3(meshlet.x));
    float3 quantMax = float3(unpackBytes3(meshlet.y));

    float3 meshletMin = lodBboxMin.xyz + quantMin / 255.0 * extent;
    float3 meshletMax = lodBboxMin.xyz + quantMax / 255.0 * extent;

    float3 center = (meshletMin + meshletMax) * 0.5;
    float radius = uniformScale * 0.5 * length((quantMax - quantMin) / 255.0 * extent);

    float3 worldCenter = mul(instanceMat, float4(center, 1.0)) + sectorOffset;

    return IsSphereFrustumCulled(worldCenter, radius);
}

// Payload byte offsets (2018 NVAPI task output layout, see AsteroidTaskPayload).
#define ASTEROID_PAYLOAD_OFFSET_INDEX   0   // uint2 {asteroidIndex, numMeshTasks}
#define ASTEROID_PAYLOAD_OFFSET_LOD     8   // float4 {lod, lodAlpha, distanceAlpha, numMeshletsFirstLod}

// ---------------------------------------------------------------------------------------------
// Task -> mesh payload of the asteroid pipelines.
// The byte offsets are the ones the 2018 shaders used with the NVAPI task-output ops
// (see meshlet_shaders.NOTES.md): 2 x uint at byte 0, 4 x float at byte 8.
// ---------------------------------------------------------------------------------------------

struct AsteroidTaskPayload
{
    uint  asteroidIndex;        //  0: index into instanceInfoBuffer (task shader SV_GroupID.x)
    uint  numMeshTasks;         //  4: number of mesh groups launched for this asteroid
    float lod;                  //  8: continuous LOD; floor() = first LOD, ceil() = second LOD
    float lodAlpha;             // 12: cross-fade factor between the two LODs (0 = single LOD)
    float distanceAlpha;        // 16: view-distance fade
    float numMeshletsFirstLod;  // 20: lodInfoBuffer[floor(lod)].numMinfo, stored as float
};

// basicTS -> basicMS payload. The 2018 NVAPI basic mesh shader read no payload, but SM 6.5 requires the
// amplification and mesh shaders of a pipeline to declare the same payload size.
struct BasicTaskPayload
{
    uint unused;
};

#endif // MESHLET_COMMON_HLSLI

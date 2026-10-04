/*
 * asteroidMS.hlsl - mesh shader of the asteroid meshlet pipeline.
 *
 * Launched by asteroidTS.hlsl, one group per meshlet. Mesh group index g selects
 *   g <  numMeshletsFirstLod : meshlet g                         of LOD floor(payload.lod)
 *   g >= numMeshletsFirstLod : meshlet g - numMeshletsFirstLod   of LOD ceil(payload.lod)
 * Thread 0 culls the meshlet's quantized bounding sphere against the frustum; the result is
 * broadcast to the whole group. Surviving meshlets emit up to 64 vertices / 100 triangles.
 *
 * Permutations: DEPTH_PRE_PASS={0,1}. The depth pre-pass only outputs SV_Position.
 *
 * Ported from the 2018 NVAPI mesh-shader extension to SM 6.5 (ms_6_5); see
 * meshlet_shaders.NOTES.md for the opcode mapping.
 */

#include "include/meshlet_common.hlsli"

#ifndef DEPTH_PRE_PASS
#define DEPTH_PRE_PASS 0
#endif

#if DEPTH_PRE_PASS
struct DepthOutput
{
    float4 i_position : SV_POSITION;
};
#define VertexOutput DepthOutput
#else
// Consumed by gbuffer_ps / forward_ps / material_id_ps with _ASTEROIDS=1.
struct Output
{
    float4 i_position : SV_POSITION;
    float4 attr1 : ATTR1;                       // xyz = world position, w = u
    float4 attr2 : ATTR2;                       // xyz = world normal (not normalized), w = 1 - v
    nointerpolation float4 alphalod : ALPHA_LOD;// x = LOD cross-fade alpha, y = LOD index within the pair (0/1),
                                                // z = view-distance alpha, w = continuous LOD
};
#define VertexOutput Output
#endif

// earlyCullUsingSphere: include/meshlet_common.hlsli (shared with nvapi/asteroidMS.hlsl).

[outputtopology("triangle")]
[numthreads(MESHLET_GROUP_SIZE, 1, 1)]
void ms_main(
    uint3 groupId : SV_GroupID,
    uint3 groupThreadId : SV_GroupThreadID,
    in payload AsteroidTaskPayload payload,
    out vertices VertexOutput verts[MESHLET_MAX_VERTICES],
    out indices uint3 tris[MESHLET_MAX_PRIMITIVES])
{
    uint laneId = groupThreadId.x;
    uint taskIndex = groupId.x;

    uint numMeshletsFirstLod = uint(payload.numMeshletsFirstLod);
    bool isFirstLod = taskIndex < numMeshletsFirstLod;
    uint lodIndex = isFirstLod ? 0 : 1;

#if !DEPTH_PRE_PASS
    float alpha = (cbFrame.enableLod != 0) ? payload.lodAlpha : 0;
#endif

    uint meshletIndexInLod = isFirstLod ? taskIndex : taskIndex - numMeshletsFirstLod;
    uint lod = uint(isFirstLod ? floor(payload.lod) : ceil(payload.lod));

    LODInfo lodInfo = lodInfoBuffer[lod];
    AsteroidInstance instance = instanceInfoBuffer[payload.asteroidIndex];
    uint4 meshlet = meshletBuffer[lodInfo.minfoStart + meshletIndexInLod];

    uint render = 1;
    if (laneId == 0)
    {
        BoundingBox lodBbox = cbObjectInfo.bboxLods[lod];
        render = earlyCullUsingSphere(meshlet, instance.instanceMat, instance.uniformScale,
                                      cbSectorInfo.sectorOffset, lodBbox.bboxMin, lodBbox.bboxMax) ? 0 : 1;
    }
    render = WaveReadLaneAt(render, 0);

    uint vertexCount = getMeshletVertexCount(meshlet);
    uint primCount = getMeshletPrimitiveCount(meshlet);

    SetMeshOutputCounts(render ? vertexCount : 0, render ? primCount : 0);

    if (render == 0)
        return;

    uint vertexOffset = meshlet.z + lodInfo.indexStart;
    uint primOffset = meshlet.w + lodInfo.primStart;

    for (uint i = laneId; i < vertexCount; i += MESHLET_GROUP_SIZE)
    {
        uint vertexIndex = indexBuffer[vertexOffset + i] + lodInfo.vertexStart;

        float3 position = vertexPositionBuffer[vertexIndex];
        float3 worldPos = mul(instance.instanceMat, float4(position, 1.0)) + cbSectorInfo.sectorOffset;

        VertexOutput o;
        o.i_position = mul(float4(worldPos, 1.0), cbFrame.matWorldToClip);

#if !DEPTH_PRE_PASS
        float2 uv = vertexTexcoord1Buffer[vertexIndex];
        float3 normal = unpackSnorm8x3(vertexNormalBuffer[vertexIndex]);
        float3 worldNormal = mul(instance.instanceMat, float4(normal, 0.0));

        o.attr1 = float4(worldPos, uv.x);
        o.attr2 = float4(worldNormal, 1.0 - uv.y);
        o.alphalod = float4(alpha, float(lodIndex), payload.distanceAlpha, payload.lod);
#endif

        verts[i] = o;
    }

    for (uint p = laneId; p < primCount; p += MESHLET_GROUP_SIZE)
    {
        uint i0, i1, i2;
        getMeshletPrimIndices(primOffset + p * 3, i0, i1, i2);
        tris[p] = uint3(i0, i1, i2);
    }
}

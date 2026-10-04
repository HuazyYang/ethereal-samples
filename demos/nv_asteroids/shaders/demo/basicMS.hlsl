/*
 * basicMS.hlsl - mesh shader of the generic (non-asteroid) meshlet pipeline (e.g. the ship).
 *
 * One group per meshlet: meshlet = meshletBuffer[cbMeshletInfo.firstMeshlet + SV_GroupID.x].
 * Vertices are transformed with cbInstance (current frame) and cbInstancePrev (previous frame,
 * for motion vectors); normals/tangents/bitangents are snorm8x3 decoded by hand.
 *
 * Permutations: DEPTH_PRE_PASS={0,1}.
 *
 * Ported from the 2018 NVAPI mesh-shader extension to SM 6.5 (ms_6_5); see
 * meshlet_shaders.NOTES.md for the opcode mapping.
 */

#include "include/meshlet_common.hlsli"

#ifndef DEPTH_PRE_PASS
#define DEPTH_PRE_PASS 0
#endif

#if DEPTH_PRE_PASS
// The 2018 shader declared DepthOutput { i_position } but also wrote the texture coordinate
// to attribute slot 1 (the register right after SV_Position); kept here as UV.
struct DepthOutput
{
    float4 i_position : SV_POSITION;
    float2 m_uv : UV;
};
#define VertexOutput DepthOutput
#else
// Consumed by gbuffer_ps / forward_ps / material_id_ps with _ASTEROIDS=0.
struct Output
{
    float4 i_position : SV_POSITION;
    float3 m_pos : POS;
    float2 m_uv : UV;
    centroid float3 m_normal : NORMAL;
    centroid float3 m_tangent : TANGENT;
    centroid float3 m_bitangent : BITANGENT;
    float3 m_prevPos : PREV_WORLD_POS;
};
#define VertexOutput Output
#endif

[outputtopology("triangle")]
[numthreads(MESHLET_GROUP_SIZE, 1, 1)]
void ms_main(
    uint3 groupId : SV_GroupID,
    uint3 groupThreadId : SV_GroupThreadID,
    in payload BasicTaskPayload payload,  // unused; must match basicTS's payload size (SM 6.5)
    out vertices VertexOutput verts[MESHLET_MAX_VERTICES],
    out indices uint3 tris[MESHLET_MAX_PRIMITIVES])
{
    uint laneId = groupThreadId.x;
    uint4 meshlet = meshletBuffer[cbMeshletInfo.firstMeshlet + groupId.x];

    uint vertexCount = getMeshletVertexCount(meshlet);
    uint primCount = getMeshletPrimitiveCount(meshlet);

    SetMeshOutputCounts(vertexCount, primCount);

    for (uint i = laneId; i < vertexCount; i += MESHLET_GROUP_SIZE)
    {
        uint vertexIndex = indexBuffer[meshlet.z + i];

        float3 position = vertexPositionBuffer[vertexIndex];
        float3 worldPos = mul(cbInstance.instanceMat, float4(position, 1.0));

        VertexOutput o;
        o.i_position = mul(float4(worldPos, 1.0), cbFrame.matWorldToClip);
        o.m_uv = vertexTexcoord1Buffer[vertexIndex];

#if !DEPTH_PRE_PASS
        float3 normal = unpackSnorm8x3(vertexNormalBuffer[vertexIndex]);
        float3 tangent = unpackSnorm8x3(vertexTangentBuffer[vertexIndex]);
        float3 bitangent = unpackSnorm8x3(vertexBitangentBuffer[vertexIndex]);

        o.m_pos = worldPos;
        o.m_normal = mul(cbInstance.instanceMat, float4(normal, 0.0));
        o.m_tangent = mul(cbInstance.instanceMat, float4(tangent, 0.0));
        o.m_bitangent = mul(cbInstance.instanceMat, float4(bitangent, 0.0));
        o.m_prevPos = mul(cbInstancePrev.instanceMat, float4(position, 1.0));
#endif

        verts[i] = o;
    }

    for (uint p = laneId; p < primCount; p += MESHLET_GROUP_SIZE)
    {
        uint i0, i1, i2;
        getMeshletPrimIndices(meshlet.w + p * 3, i0, i1, i2);
        tris[p] = uint3(i0, i1, i2);
    }
}

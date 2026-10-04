/*
 * nvapi/particles.hlsl - the ORIGINAL 2018 NVAPI particle mesh shader (vs_6_0, entry ms_main; the shipped blob
 * is particles.hlsl:ms_main). Same algorithm as ../particles_ms.hlsl: 32 particles per group, culling, ballot
 * compaction (NvBallot), one triangle per visible particle. g_NvidiaExt is u0 here.
 * The PSO has a mesh shader extension only (no task shader): 96 vertices / 32 primitives per group.
 */

#define NV_SHADER_EXTN_SLOT u0
#include "nv_meshlet_extns.hlsli"
#include "../include/particles_ms_common.hlsli"

void ms_body()
{
    uint laneId = __NvMeshGetThreadId();
    uint groupId = __NvMeshGetGroupId();
    uint particleIndex = groupId * PARTICLES_GROUP_SIZE + laneId;

    ParticleInfo particle = t_Particles[particleIndex];

    float3 position = particle.position + g_Particles.positionOffset;
    float distance = length(position);
    float4 clipPos = mul(float4(position, 1.0), g_Particles.matWorldToClip);

    bool visible = clipPos.w > 0
        && distance <= g_Particles.maxDistance
        && abs(clipPos.x) <= clipPos.w
        && abs(clipPos.y) <= clipPos.w;

    uint visibleMask = NvBallot(visible);

    if (visible)
    {
        float2 size;
        float4 color;
        SetupParticle(position, clipPos, particle.brightness, distance, size, color);

        uint primIndex = countbits(visibleMask & ((1u << (laneId & 31)) - 1));

        for (uint vertexId = 0; vertexId < 3; vertexId++)
        {
            uint vertex = primIndex * 3 + vertexId;
            __NvMeshSetPosition(vertex, float4(clipPos.xy + size * TrianglePoints[vertexId], clipPos.zw));
            __NvMeshSetVertexAttribute(vertex, 1, color);
            __NvMeshSetVertexAttribute(vertex, 2, float4(TrianglePoints[vertexId], 0, 0));
            __NvMeshSetPrimitiveIndex(vertex, vertex);
        }
    }

    __NvMeshCommitOutputs();

    if (laneId == 0)
        __NvMeshSetPrimitiveCount(countbits(visibleMask));
}

PS_Input ms_main()
{
    ms_body();

    PS_Input output;
    output.position = 12345.0;
    output.color = 12345.0;
    output.uv = 12345.0;
    return output;
}

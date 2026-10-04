/*
 * particles_ms.hlsl - mesh shader that renders the space-dust particles, one screen-aligned
 * triangle per visible particle (no task shader).
 *
 * Group g handles particles [g * 32, g * 32 + 32) of t_Particles. Each thread culls its particle
 * (behind the camera, beyond maxDistance, outside the x/y clip range), the survivors are
 * compacted with a wave ballot, and each visible particle emits 3 vertices / 1 triangle with
 * colour = lit, shadowed (cascaded shadow maps) and distance-faded brightness.
 *
 * Ported from the 2018 NVAPI mesh-shader extension to SM 6.5 (ms_6_5); see
 * meshlet_shaders.NOTES.md for the opcode mapping.
 */

#include "include/particles_ms_common.hlsli"


[outputtopology("triangle")]
[numthreads(PARTICLES_GROUP_SIZE, 1, 1)]
void ms_main(
    uint3 groupId : SV_GroupID,
    uint3 groupThreadId : SV_GroupThreadID,
    out vertices PS_Input verts[PARTICLES_GROUP_SIZE * 3],
    out indices uint3 tris[PARTICLES_GROUP_SIZE])
{
    uint laneId = groupThreadId.x;
    uint particleIndex = groupId.x * PARTICLES_GROUP_SIZE + laneId;

    ParticleInfo particle = t_Particles[particleIndex];

    float3 position = particle.position + g_Particles.positionOffset;
    float distance = length(position);
    float4 clipPos = mul(float4(position, 1.0), g_Particles.matWorldToClip);

    bool visible = clipPos.w > 0
        && distance <= g_Particles.maxDistance
        && abs(clipPos.x) <= clipPos.w
        && abs(clipPos.y) <= clipPos.w;

    // NvBallot + countbits in the original; one triangle per visible particle, compacted.
    uint numVisible = WaveActiveCountBits(visible);
    uint primIndex = WavePrefixCountBits(visible);

    SetMeshOutputCounts(numVisible * 3, numVisible);

    if (!visible)
        return;

    float2 size;
    float4 color;
    SetupParticle(position, clipPos, particle.brightness, distance, size, color);

    for (uint vertexId = 0; vertexId < 3; vertexId++)
    {
        PS_Input o;
        o.position = float4(clipPos.xy + size * TrianglePoints[vertexId], clipPos.zw);
        o.color = color;
        o.uv = TrianglePoints[vertexId];
        verts[primIndex * 3 + vertexId] = o;
    }

    tris[primIndex] = uint3(primIndex * 3 + 0, primIndex * 3 + 1, primIndex * 3 + 2);
}

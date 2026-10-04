/*
 * debugTS.hlsl - task (amplification) shader of the asteroid bounding-box debug pipeline
 * (cbFrame.showBBoxes).
 *
 * Same per-asteroid evaluation as asteroidTS.hlsl (frustum culling, view-distance fade, LOD)
 * so that the payload carries the LOD information, but it launches exactly one mesh group
 * (debugMS.hlsl) per asteroid whenever showBBoxes is set - also for culled asteroids, in which
 * case the floating-point part of the payload is left unwritten, as in the original.
 *
 * Permutations: DEPTH_PRE_PASS=0, _MESHLETS_HI_Z=0 (the only ones shipped).
 *
 * Ported from the 2018 NVAPI mesh-shader extension to SM 6.5 (as_6_5); see
 * meshlet_shaders.NOTES.md for the opcode mapping.
 */

#include "include/meshlet_common.hlsli"

groupshared AsteroidTaskPayload s_Payload;
groupshared uint s_NumMeshTasks;

[numthreads(MESHLET_GROUP_SIZE, 1, 1)]
void ts_main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    uint asteroidIndex = groupId.x;
    uint laneId = groupThreadId.x;

    if (laneId == 0)
    {
        AsteroidInstance instance = instanceInfoBuffer[asteroidIndex];

        float3 center = mul(instance.instanceMat, float4(cbObjectInfo.center, 1.0)) + cbSectorInfo.sectorOffset;
        float radius = cbObjectInfo.radius * instance.uniformScale;

        bool culled = (cbFrame.enableAsteroidsCulling != 0) && IsSphereFrustumCulled(center, radius);

        if (!culled)
        {
            float3 viewVector = center - cbFrame.cameraPos.xyz;

            float distanceAlpha = 1.0;
            bool fadedOut = false;
            if (cbFrame.enableViewDistanceFade != 0)
            {
                distanceAlpha = computeViewDistanceFade(viewVector, radius);
                fadedOut = distanceAlpha <= 0.0001;
            }

            if (!fadedOut)
            {
                float fLod;
                if (cbFrame.enableLod != 0)
                    fLod = computeLOD(viewVector, radius);
                else
                    fLod = clamp(float(cbFrame.forcedLod), 0, cbObjectInfo.maxLevelToRender);

                float alpha = 0;
                if (cbFrame.transitionRange > 0)
                {
                    float range = min(max(cbFrame.transitionRange, 0), 1);
                    alpha = saturate((frac(fLod) - (1.0 - range)) / range);
                    fLod = floor(fLod) + alpha;
                }
                else
                {
                    fLod = floor(fLod);
                }

                s_Payload.lod = fLod;
                s_Payload.lodAlpha = alpha;
                s_Payload.distanceAlpha = distanceAlpha;
                s_Payload.numMeshletsFirstLod = float(lodInfoBuffer[uint(floor(fLod))].numMinfo);
            }
        }

        uint numMeshTasks = (cbFrame.showBBoxes != 0) ? 1 : 0;

        s_Payload.asteroidIndex = asteroidIndex;
        s_Payload.numMeshTasks = numMeshTasks;
        s_NumMeshTasks = numMeshTasks;
    }

    GroupMemoryBarrierWithGroupSync();

    DispatchMesh(s_NumMeshTasks, 1, 1, s_Payload);
}

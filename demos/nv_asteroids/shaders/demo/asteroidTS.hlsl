/*
 * asteroidTS.hlsl - task (amplification) shader of the asteroid meshlet pipeline.
 *
 * One task group per asteroid instance (SV_GroupID.x indexes instanceInfoBuffer). Thread 0
 * does all the work, as in the original NVAPI shader:
 *   1. bounding-sphere frustum culling (cbFrame.enableAsteroidsCulling),
 *   2. view-distance fade (cbFrame.enableViewDistanceFade),
 *   3. optional Hi-Z occlusion test of the object bounding box (_MESHLETS_HI_Z=1),
 *   4. continuous LOD selection with an optional cross-fade between two LODs,
 * and launches one mesh group per meshlet of the selected LOD(s) (asteroidMS.hlsl).
 *
 * Permutations: DEPTH_PRE_PASS={0,1} (only changed the dummy vertex-shader output signature of
 * the 2018 binary; no effect here), _MESHLETS_HI_Z={0,1}.
 *
 * Ported from the 2018 NVAPI mesh-shader extension to SM 6.5 (as_6_5); see
 * meshlet_shaders.NOTES.md for the opcode mapping.
 */

#include "include/meshlet_common.hlsli"

#ifndef _MESHLETS_HI_Z
#define _MESHLETS_HI_Z 0
#endif

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

        uint numMeshTasks = 0;

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
#if _MESHLETS_HI_Z
                if (IsOcclusionCulled(instance.instanceMat, asteroidIndex))
                {
                    if (cbFrame.enableZCullStats != 0)
                        InterlockedAdd(u_Stats[0], 1);
                }
                else
#endif
                {
                    float fLod;
                    if (cbFrame.enableLod != 0)
                        fLod = computeLOD(viewVector, radius);
                    else
                        fLod = clamp(float(cbFrame.forcedLod), 0, cbObjectInfo.maxLevelToRender);

                    // Cross-fade between floor(fLod) and floor(fLod) + 1 in the last
                    // 'transitionRange' fraction of each LOD interval.
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

                    uint numLods = (alpha == 0) ? 1 : 2;
                    float firstLod = floor(fLod);

                    s_Payload.lod = fLod;
                    s_Payload.lodAlpha = alpha;
                    s_Payload.distanceAlpha = distanceAlpha;
                    s_Payload.numMeshletsFirstLod = float(lodInfoBuffer[uint(firstLod)].numMinfo);

                    for (uint lodIndex = 0; lodIndex < numLods; lodIndex++)
                        numMeshTasks += lodInfoBuffer[uint(firstLod + float(lodIndex))].numMinfo;
                }
            }
        }

        s_Payload.asteroidIndex = asteroidIndex;
        s_Payload.numMeshTasks = numMeshTasks;
        s_NumMeshTasks = numMeshTasks;
    }

    GroupMemoryBarrierWithGroupSync();

    // One mesh group per meshlet; groups [0, numMeshletsFirstLod) draw the first LOD.
    DispatchMesh(s_NumMeshTasks, 1, 1, s_Payload);
}

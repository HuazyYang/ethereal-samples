/*
 * nvapi/asteroidTS.hlsl - the ORIGINAL 2018 NVAPI form of the asteroid task shader (vs_6_0, entry ts_main).
 *
 * Same algorithm as ../asteroidTS.hlsl (the SM 6.5 port): one task group per asteroid instance, thread 0
 * culls (frustum, view-distance fade, optional Hi-Z), selects the LOD(s), writes the payload and the number of
 * mesh groups. Here everything goes through the NVAPI extension ops on g_NvidiaExt (u7); see
 * nv_meshlet_extns.hlsli. Reproduces asteroidTS_ts_main (DEPTH_PRE_PASS x _MESHLETS_HI_Z) of the shipped blob.
 */

#define NV_SHADER_EXTN_SLOT u7
#include "nv_meshlet_extns.hlsli"
#include "../include/meshlet_common.hlsli"

#define NV_OUTPUT_ASTEROIDS 1
#include "nv_outputs.hlsli"

#ifndef _MESHLETS_HI_Z
#define _MESHLETS_HI_Z 0
#endif

VertexOutput ts_main()
{
    uint asteroidIndex = __NvMeshGetGroupId();
    AsteroidInstance instance = instanceInfoBuffer[asteroidIndex];

    uint laneId = __NvMeshGetThreadId();
    if (laneId == 0)
    {
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

                    __NvMeshWriteTaskOutputFloat4(ASTEROID_PAYLOAD_OFFSET_LOD,
                        float4(fLod, alpha, distanceAlpha, float(lodInfoBuffer[uint(firstLod)].numMinfo)));

                    for (uint lodIndex = 0; lodIndex < numLods; lodIndex++)
                        numMeshTasks += lodInfoBuffer[uint(firstLod + float(lodIndex))].numMinfo;
                }
            }
        }

        __NvMeshWriteTaskOutputUint2(ASTEROID_PAYLOAD_OFFSET_INDEX, uint2(asteroidIndex, numMeshTasks));
        __NvMeshSetTaskCount(numMeshTasks);
    }

    return DummyOutput();
}

/*
 * nvapi/debugTS.hlsl - the ORIGINAL 2018 NVAPI form of the bounding-box debug task shader (vs_6_0, ts_main).
 * Same algorithm as ../debugTS.hlsl. Reproduces debugTS_ts_main (DEPTH_PRE_PASS=0, _MESHLETS_HI_Z=0).
 */

#define NV_SHADER_EXTN_SLOT u7
#include "nv_meshlet_extns.hlsli"
#include "../include/meshlet_common.hlsli"

#define NV_OUTPUT_ASTEROIDS 0
#include "nv_outputs.hlsli"

VertexOutput ts_main()
{
    uint asteroidIndex = __NvMeshGetGroupId();
    AsteroidInstance instance = instanceInfoBuffer[asteroidIndex];

    uint laneId = __NvMeshGetThreadId();
    if (laneId == 0)
    {
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

                __NvMeshWriteTaskOutputFloat4(ASTEROID_PAYLOAD_OFFSET_LOD,
                    float4(fLod, alpha, distanceAlpha, float(lodInfoBuffer[uint(floor(fLod))].numMinfo)));
            }
        }

        uint numMeshTasks = (cbFrame.showBBoxes != 0) ? 1 : 0;

        __NvMeshWriteTaskOutputUint2(ASTEROID_PAYLOAD_OFFSET_INDEX, uint2(asteroidIndex, numMeshTasks));
        __NvMeshSetTaskCount(numMeshTasks);
    }

    return DummyOutput();
}

/*
 * nvapi/debugMS.hlsl - the ORIGINAL 2018 NVAPI form of the bounding-box debug mesh shader (vs_6_0, ms_main).
 * Same algorithm and attribute slots as ../debugMS.hlsl (DrawAsteroidsBBox: 8 vertices, 12 triangles, the
 * per-primitive LOD data in slot 4, the previous position in slot 5). Reproduces debugMS_ms_main.
 */

#define NV_SHADER_EXTN_SLOT u7
#include "nv_meshlet_extns.hlsli"
#include "../include/meshlet_common.hlsli"

#define NV_OUTPUT_ASTEROIDS 0
#include "nv_outputs.hlsli"

#define BBOX_NUM_VERTICES   8
#define BBOX_NUM_TRIANGLES  12

// Returns the number of triangles (2018 DrawAsteroidsBBox(uint, uint, uint, float4)).
uint DrawAsteroidsBBox(uint laneId, uint asteroidIndex, uint lodIndex, float4 lodData)
{
    static const uint bboxIB[BBOX_NUM_TRIANGLES * 3] =
    {
        0, 1, 2,   2, 1, 3,
        0, 4, 5,   0, 5, 1,
        1, 5, 7,   1, 7, 3,
        2, 7, 6,   3, 7, 2,
        0, 2, 6,   0, 6, 4,
        4, 6, 5,   5, 6, 7
    };

    AsteroidInstance instance = instanceInfoBuffer[asteroidIndex];

    if (laneId < BBOX_NUM_VERTICES)
    {
        float4 corner = getBoxCorner(cbObjectInfo.bbox.bboxMin.xyz, cbObjectInfo.bbox.bboxMax.xyz, laneId);
        float3 worldPos = mul(instance.instanceMat, corner) + cbSectorInfo.sectorOffset;
        float3 prevWorldPos = worldPos + cbFrame.preViewTranslationPrevious.xyz - cbFrame.preViewTranslation.xyz;
        float4 viewPos = mul(float4(worldPos, 1.0), cbFrame.matWorldToView);

        __NvMeshSetPosition(laneId, mul(viewPos, cbFrame.matViewToClip));
        __NvMeshSetVertexAttribute(laneId, NV_SLOT_POS, float4(worldPos, 1.0));
        __NvMeshSetVertexAttribute(laneId, NV_SLOT_UV, float4(1.0, 0.0, 0.0, 0.0));
        __NvMeshSetVertexAttribute(laneId, NV_SLOT_NORMAL, float4(1.0, 0.0, 0.0, 0.0));
        __NvMeshSetVertexAttribute(laneId, NV_SLOT_BITANGENT, float4(prevWorldPos, 1.0));
    }
    else if (laneId < BBOX_NUM_VERTICES + BBOX_NUM_TRIANGLES)
    {
        uint triangleIndex = laneId - BBOX_NUM_VERTICES;
        __NvMeshSetPrimitiveAttribute(triangleIndex, NV_SLOT_TANGENT,
            float4(lodData.y, float(lodIndex), lodData.z, lodData.x));
        __NvMeshSetPrimitiveIndex(triangleIndex * 3 + 0, bboxIB[triangleIndex * 3 + 0]);
        __NvMeshSetPrimitiveIndex(triangleIndex * 3 + 1, bboxIB[triangleIndex * 3 + 1]);
        __NvMeshSetPrimitiveIndex(triangleIndex * 3 + 2, bboxIB[triangleIndex * 3 + 2]);
    }

    return BBOX_NUM_TRIANGLES;
}

void ms_body()
{
    uint laneId = __NvMeshGetThreadId();
    uint taskIndex = __NvMeshGetGroupId();

    uint asteroidIndex = __NvMeshReadTaskInputUint(ASTEROID_PAYLOAD_OFFSET_INDEX);
    float4 lodData = __NvMeshReadTaskInputFloat4(ASTEROID_PAYLOAD_OFFSET_LOD);
    uint numMeshTasks = __NvMeshReadTaskInputUint(ASTEROID_PAYLOAD_OFFSET_INDEX + 4);

    if (cbFrame.showBBoxes != 0 && taskIndex == numMeshTasks - 1)
    {
        uint lodIndex = (taskIndex >= uint(lodData.w)) ? 1 : 0;
        uint numPrimitives = DrawAsteroidsBBox(laneId, asteroidIndex, lodIndex, lodData);
        __NvMeshCommitOutputs();
        __NvMeshSetPrimitiveCount(numPrimitives);
    }
}

VertexOutput ms_main()
{
    ms_body();
    return DummyOutput();
}

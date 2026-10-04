/*
 * nvapi/asteroidMS.hlsl - the ORIGINAL 2018 NVAPI form of the asteroid mesh shader (vs_6_0, entry ms_main).
 *
 * Same algorithm as ../asteroidMS.hlsl (the SM 6.5 port); the payload, thread / group ids, vertex and index
 * outputs and the primitive count go through the NVAPI extension ops (nv_meshlet_extns.hlsli). Thread 0's
 * meshlet-sphere culling result is broadcast with NvShfl. Reproduces asteroidMS_ms_main (DEPTH_PRE_PASS).
 */

#define NV_SHADER_EXTN_SLOT u7
#include "nv_meshlet_extns.hlsli"
#include "../include/meshlet_common.hlsli"

#define NV_OUTPUT_ASTEROIDS 1
#include "nv_outputs.hlsli"

void ms_body()
{
    uint laneId = __NvMeshGetThreadId();
    uint taskIndex = __NvMeshGetGroupId();

    uint asteroidIndex = __NvMeshReadTaskInputUint(ASTEROID_PAYLOAD_OFFSET_INDEX);
    float4 lodData = __NvMeshReadTaskInputFloat4(ASTEROID_PAYLOAD_OFFSET_LOD);
    float payloadLod = lodData.x;
    float payloadLodAlpha = lodData.y;
    float payloadDistanceAlpha = lodData.z;

    uint numMeshletsFirstLod = uint(lodData.w);
    bool isFirstLod = taskIndex < numMeshletsFirstLod;
    uint lodIndex = isFirstLod ? 0 : 1;

#if !DEPTH_PRE_PASS
    float alpha = (cbFrame.enableLod != 0) ? payloadLodAlpha : 0;
#endif

    uint meshletIndexInLod = isFirstLod ? taskIndex : taskIndex - numMeshletsFirstLod;
    uint lod = uint(isFirstLod ? floor(payloadLod) : ceil(payloadLod));

    LODInfo lodInfo = lodInfoBuffer[lod];
    AsteroidInstance instance = instanceInfoBuffer[asteroidIndex];
    uint4 meshlet = meshletBuffer[lodInfo.minfoStart + meshletIndexInLod];

    uint render = 1;
    if (laneId == 0)
    {
        BoundingBox lodBbox = cbObjectInfo.bboxLods[lod];
        render = earlyCullUsingSphere(meshlet, instance.instanceMat, instance.uniformScale,
                                      cbSectorInfo.sectorOffset, lodBbox.bboxMin, lodBbox.bboxMax) ? 0 : 1;
    }
    render = NvShfl(render, 0);

    if (render == 0)
    {
        if (laneId == 0)
            __NvMeshSetPrimitiveCount(0);
        return;
    }

    uint vertexOffset = meshlet.z + lodInfo.indexStart;
    uint primOffset = meshlet.w + lodInfo.primStart;
    uint vertexCount = getMeshletVertexCount(meshlet);
    uint primCount = getMeshletPrimitiveCount(meshlet);

    for (uint i = laneId; i < vertexCount; i += MESHLET_GROUP_SIZE)
    {
        uint vertexIndex = indexBuffer[vertexOffset + i] + lodInfo.vertexStart;

        float3 position = vertexPositionBuffer[vertexIndex];
        float3 worldPos = mul(instance.instanceMat, float4(position, 1.0)) + cbSectorInfo.sectorOffset;

        __NvMeshSetPosition(i, mul(float4(worldPos, 1.0), cbFrame.matWorldToClip));

#if !DEPTH_PRE_PASS
        float2 uv = vertexTexcoord1Buffer[vertexIndex];
        float3 normal = unpackSnorm8x3(vertexNormalBuffer[vertexIndex]);
        float3 worldNormal = mul(instance.instanceMat, float4(normal, 0.0));

        __NvMeshSetVertexAttribute(i, NV_SLOT_ATTR1, float4(worldPos, uv.x));
        __NvMeshSetVertexAttribute(i, NV_SLOT_ATTR2, float4(worldNormal, 1.0 - uv.y));
        __NvMeshSetVertexAttribute(i, NV_SLOT_ALPHA_LOD, float4(alpha, float(lodIndex), payloadDistanceAlpha, payloadLod));
#endif
    }

    for (uint p = laneId; p < primCount; p += MESHLET_GROUP_SIZE)
    {
        uint i0, i1, i2;
        getMeshletPrimIndices(primOffset + p * 3, i0, i1, i2);
        __NvMeshSetPrimitiveIndex(p * 3 + 0, i0);
        __NvMeshSetPrimitiveIndex(p * 3 + 1, i1);
        __NvMeshSetPrimitiveIndex(p * 3 + 2, i2);
    }

    __NvMeshCommitOutputs();

    if (laneId == 0)
        __NvMeshSetPrimitiveCount(primCount);
}

VertexOutput ms_main()
{
    ms_body();
    return DummyOutput();
}

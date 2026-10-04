/*
 * nvapi/basicMS.hlsl - the ORIGINAL 2018 NVAPI form of the basic-object mesh shader (vs_6_0, entry ms_main).
 * One mesh group per meshlet of the draw (cbMeshletInfo); same algorithm as ../basicMS.hlsl.
 * Reproduces basicMS_ms_main (DEPTH_PRE_PASS).
 */

#define NV_SHADER_EXTN_SLOT u7
#include "nv_meshlet_extns.hlsli"
#include "../include/meshlet_common.hlsli"

#define NV_OUTPUT_ASTEROIDS 0
#include "nv_outputs.hlsli"

void ms_body()
{
    uint laneId = __NvMeshGetThreadId();
    uint groupId = __NvMeshGetGroupId();

    uint4 meshlet = meshletBuffer[cbMeshletInfo.firstMeshlet + groupId];
    uint vertexCount = getMeshletVertexCount(meshlet);
    uint primCount = getMeshletPrimitiveCount(meshlet);

    for (uint i = laneId; i < vertexCount; i += MESHLET_GROUP_SIZE)
    {
        uint vertexIndex = indexBuffer[meshlet.z + i];

        float3 position = vertexPositionBuffer[vertexIndex];
        float3 worldPos = mul(cbInstance.instanceMat, float4(position, 1.0));

        __NvMeshSetPosition(i, mul(float4(worldPos, 1.0), cbFrame.matWorldToClip));

        float2 uv = vertexTexcoord1Buffer[vertexIndex];

#if DEPTH_PRE_PASS
        // The depth variant still writes the texture coordinate to slot 1.
        __NvMeshSetVertexAttribute(i, 1, float4(uv, 0, 0));
#else
        float3 normal = unpackSnorm8x3(vertexNormalBuffer[vertexIndex]);
        float3 tangent = unpackSnorm8x3(vertexTangentBuffer[vertexIndex]);
        float3 bitangent = unpackSnorm8x3(vertexBitangentBuffer[vertexIndex]);
        float3 prevWorldPos = mul(cbInstancePrev.instanceMat, float4(position, 1.0));

        __NvMeshSetVertexAttribute(i, NV_SLOT_POS, float4(worldPos, 1.0));
        __NvMeshSetVertexAttribute(i, NV_SLOT_UV, float4(uv, 0, 0));
        __NvMeshSetVertexAttribute(i, NV_SLOT_NORMAL, float4(mul(cbInstance.instanceMat, float4(normal, 0.0)), 0));
        __NvMeshSetVertexAttribute(i, NV_SLOT_TANGENT, float4(mul(cbInstance.instanceMat, float4(tangent, 0.0)), 0));
        __NvMeshSetVertexAttribute(i, NV_SLOT_BITANGENT, float4(mul(cbInstance.instanceMat, float4(bitangent, 0.0)), 0));
        __NvMeshSetVertexAttribute(i, NV_SLOT_PREV_POS, float4(prevWorldPos, 1.0));
#endif
    }

    for (uint p = laneId; p < primCount; p += MESHLET_GROUP_SIZE)
    {
        uint i0, i1, i2;
        getMeshletPrimIndices(meshlet.w + p * 3, i0, i1, i2);
        __NvMeshSetPrimitiveIndex(p * 3 + 0, i0);
        __NvMeshSetPrimitiveIndex(p * 3 + 1, i1);
        __NvMeshSetPrimitiveIndex(p * 3 + 2, i2);
    }

    __NvMeshCommitOutputs();
    __NvMeshSetPrimitiveCount(primCount);
}

VertexOutput ms_main()
{
    ms_body();
    return DummyOutput();
}

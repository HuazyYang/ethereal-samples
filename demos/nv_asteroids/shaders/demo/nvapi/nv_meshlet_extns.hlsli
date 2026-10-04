/*
 * nv_meshlet_extns.hlsli - the 2018 (pre-standard, Turing) NVAPI mesh-shader HLSL extension, reconstructed.
 *
 * The 2018 "task" and "mesh" shaders are vertex shaders (vs_6_0). Every mesh-shading operation is an NVAPI
 * shader-extension call on the fake UAV g_NvidiaExt (RWStructuredBuffer<NvShaderExtnStruct>):
 *     index = g_NvidiaExt.IncrementCounter();
 *     g_NvidiaExt[index].opcode = <op>; g_NvidiaExt[index].srcNu... = <operands>;
 *     result = g_NvidiaExt.IncrementCounter();      // (more results: one more IncrementCounter each)
 * The driver replaces these with the hardware mesh-shader instructions when the PSO is created through
 * NvAPI_D3D12_CreateGraphicsPipelineState with the task / mesh shader extensions (see
 * src/meshlets/NvMeshShaderPsoExt.h).
 *
 * The opcodes 34..48 are not in the public NVAPI headers (nvShaderExtnEnums.h stops at 33 and resumes at 51).
 * Their meaning is inferred from the operand usage in the shipped binaries (see meshlet_shaders.NOTES.md). The
 * store order (opcode first, then operands, src2u last) is the one in the 2018 DXIL.
 *
 * Define NV_SHADER_EXTN_SLOT before including (u7 for the meshlet shaders, u0 for the particles).
 */

#ifndef NV_MESHLET_EXTNS_HLSLI
#define NV_MESHLET_EXTNS_HLSLI

#ifndef NV_SHADER_EXTN_SLOT
#define NV_SHADER_EXTN_SLOT u7
#endif

// Public part: NvShaderExtnStruct, g_NvidiaExt, NvShfl (op 1), NvBallot (op 7).
#include "nvHLSLExtns.h"

#define NV_EXTN_OP_MESH_SET_TASK_COUNT          34  // task shader: number of mesh groups to launch
#define NV_EXTN_OP_MESH_GET_GROUP_ID            36  // index of the task / mesh group
#define NV_EXTN_OP_MESH_GET_THREAD_ID           37  // thread index within the 32-thread group
#define NV_EXTN_OP_MESH_SET_VERTEX_ATTRIBUTE    38  // per-vertex output
#define NV_EXTN_OP_MESH_SET_PRIMITIVE_ATTRIBUTE 39  // per-primitive output
#define NV_EXTN_OP_MESH_SET_PRIMITIVE_COUNT     42  // number of output primitives
#define NV_EXTN_OP_MESH_SET_PRIMITIVE_INDEX     43  // one entry of the index list (3 per triangle)
#define NV_EXTN_OP_MESH_WRITE_TASK_OUTPUT       46  // task shader: write the task -> mesh payload
#define NV_EXTN_OP_MESH_READ_TASK_INPUT         47  // mesh shader: read the payload
#define NV_EXTN_OP_MESH_COMMIT_OUTPUTS          48  // issued once after all outputs are written (inferred)

// Attribute classes of ops 38 / 39 (src0u.x).
#define NV_MESH_ATTRIBUTE_POSITION  0   // SV_Position (slot 0)
#define NV_MESH_ATTRIBUTE_GENERIC   6   // generic attribute; slot = output register of the (dummy) VS signature

// Payload element types of ops 46 / 47.
#define NV_MESH_PAYLOAD_UINT        0
#define NV_MESH_PAYLOAD_FLOAT       1

uint __NvMeshGetGroupId()
{
    uint index = g_NvidiaExt.IncrementCounter();
    g_NvidiaExt[index].opcode = NV_EXTN_OP_MESH_GET_GROUP_ID;
    return g_NvidiaExt.IncrementCounter();
}

uint __NvMeshGetThreadId()
{
    uint index = g_NvidiaExt.IncrementCounter();
    g_NvidiaExt[index].opcode = NV_EXTN_OP_MESH_GET_THREAD_ID;
    return g_NvidiaExt.IncrementCounter();
}

void __NvMeshSetTaskCount(uint count)
{
    uint index = g_NvidiaExt.IncrementCounter();
    g_NvidiaExt[index].opcode = NV_EXTN_OP_MESH_SET_TASK_COUNT;
    g_NvidiaExt[index].src0u.x = count;
    g_NvidiaExt.IncrementCounter();
}

void __NvMeshSetPrimitiveCount(uint count)
{
    uint index = g_NvidiaExt.IncrementCounter();
    g_NvidiaExt[index].opcode = NV_EXTN_OP_MESH_SET_PRIMITIVE_COUNT;
    g_NvidiaExt[index].src0u.x = count;
    g_NvidiaExt.IncrementCounter();
}

void __NvMeshCommitOutputs()
{
    uint index = g_NvidiaExt.IncrementCounter();
    g_NvidiaExt[index].opcode = NV_EXTN_OP_MESH_COMMIT_OUTPUTS;
    g_NvidiaExt.IncrementCounter();
}

// 'indexSlot' = 3 * primitive + corner.
void __NvMeshSetPrimitiveIndex(uint indexSlot, uint vertexIndex)
{
    uint index = g_NvidiaExt.IncrementCounter();
    g_NvidiaExt[index].opcode = NV_EXTN_OP_MESH_SET_PRIMITIVE_INDEX;
    g_NvidiaExt[index].src0u.x = indexSlot;
    g_NvidiaExt[index].src1u.x = vertexIndex;
    g_NvidiaExt.IncrementCounter();
}

void __NvMeshSetAttribute(uint opcode, uint attributeClass, uint slot, uint element, float4 value)
{
    uint index = g_NvidiaExt.IncrementCounter();
    g_NvidiaExt[index].opcode = opcode;
    g_NvidiaExt[index].src0u.x = attributeClass;
    g_NvidiaExt[index].src0u.y = slot;
    g_NvidiaExt[index].src0u.z = 0xff;          // component mask
    g_NvidiaExt[index].src1u.x = element;
    g_NvidiaExt[index].src2u.x = asuint(value.x);
    g_NvidiaExt[index].src2u.y = asuint(value.y);
    g_NvidiaExt[index].src2u.z = asuint(value.z);
    g_NvidiaExt[index].src2u.w = asuint(value.w);
    g_NvidiaExt.IncrementCounter();
}

void __NvMeshSetPosition(uint vertex, float4 position)
{
    __NvMeshSetAttribute(NV_EXTN_OP_MESH_SET_VERTEX_ATTRIBUTE, NV_MESH_ATTRIBUTE_POSITION, 0, vertex, position);
}

void __NvMeshSetVertexAttribute(uint vertex, uint slot, float4 value)
{
    __NvMeshSetAttribute(NV_EXTN_OP_MESH_SET_VERTEX_ATTRIBUTE, NV_MESH_ATTRIBUTE_GENERIC, slot, vertex, value);
}

void __NvMeshSetPrimitiveAttribute(uint primitive, uint slot, float4 value)
{
    __NvMeshSetAttribute(NV_EXTN_OP_MESH_SET_PRIMITIVE_ATTRIBUTE, NV_MESH_ATTRIBUTE_GENERIC, slot, primitive, value);
}

// Task output (payload) writes: 'count' 32-bit values at 'byteOffset'.
void __NvMeshWriteTaskOutput(uint byteOffset, uint type, uint count, uint4 values)
{
    uint index = g_NvidiaExt.IncrementCounter();
    g_NvidiaExt[index].opcode = NV_EXTN_OP_MESH_WRITE_TASK_OUTPUT;
    g_NvidiaExt[index].src0u.x = count;
    g_NvidiaExt[index].src0u.y = type;
    g_NvidiaExt[index].src1u.x = byteOffset;
    g_NvidiaExt[index].src2u = values;
    g_NvidiaExt.IncrementCounter();
}

void __NvMeshWriteTaskOutputUint2(uint byteOffset, uint2 values)
{
    __NvMeshWriteTaskOutput(byteOffset, NV_MESH_PAYLOAD_UINT, 2, uint4(values, 0, 0));
}

void __NvMeshWriteTaskOutputFloat4(uint byteOffset, float4 values)
{
    __NvMeshWriteTaskOutput(byteOffset, NV_MESH_PAYLOAD_FLOAT, 4, asuint(values));
}

// Task input (payload) reads; the values come back from the following IncrementCounter calls.
uint __NvMeshReadTaskInputUint(uint byteOffset)
{
    uint index = g_NvidiaExt.IncrementCounter();
    g_NvidiaExt[index].opcode = NV_EXTN_OP_MESH_READ_TASK_INPUT;
    g_NvidiaExt[index].src0u.x = NV_MESH_PAYLOAD_UINT;
    g_NvidiaExt[index].src1u.x = byteOffset;
    g_NvidiaExt[index].numOutputsForIncCounter = 1;
    return g_NvidiaExt.IncrementCounter();
}

float4 __NvMeshReadTaskInputFloat4(uint byteOffset)
{
    uint index = g_NvidiaExt.IncrementCounter();
    g_NvidiaExt[index].opcode = NV_EXTN_OP_MESH_READ_TASK_INPUT;
    g_NvidiaExt[index].src0u.x = NV_MESH_PAYLOAD_FLOAT;
    g_NvidiaExt[index].src1u.x = byteOffset;
    g_NvidiaExt[index].numOutputsForIncCounter = 4;
    float4 result;
    result.x = asfloat(g_NvidiaExt.IncrementCounter());
    result.y = asfloat(g_NvidiaExt.IncrementCounter());
    result.z = asfloat(g_NvidiaExt.IncrementCounter());
    result.w = asfloat(g_NvidiaExt.IncrementCounter());
    return result;
}

#endif // NV_MESHLET_EXTNS_HLSLI

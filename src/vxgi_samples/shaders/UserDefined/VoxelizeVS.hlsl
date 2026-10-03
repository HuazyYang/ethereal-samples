#pragma pack_matrix(row_major)

#include <donut/shaders/forward_cb.h>
#include <donut/shaders/binding_helpers.hlsli>
#include <donut/shaders/bindless.h>
#include <donut/shaders/packing.hlsli>

struct VSOutput {
    float3 pos : POS;
    float2 texCoord : TEXCOORD;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float4 posH : SV_Position;
};

DECLARE_CBUFFER(ForwardShadingViewConstants, g_ForwardView, FORWARD_BINDING_VIEW_CONSTANTS, FORWARD_SPACE_VIEW);

// Version of the vertex shader that uses the hardware Input Assembler to read vertex attributes and transforms.
void input_assembler(
    in float3 pos: POS,
    in float2 texCoord: TEXCOORD,
    in float3 normal: NORMAL,
    in float4 tangent: TANGENT,
    in float4 i_instanceMatrix0 : TRANSFORM0,
    in float4 i_instanceMatrix1 : TRANSFORM1,
    in float4 i_instanceMatrix2 : TRANSFORM2,
    out VSOutput o_vtx
)
{
    float3x4 instanceMatrix = float3x4(i_instanceMatrix0, i_instanceMatrix1, i_instanceMatrix2);

    float4 worldPos = float4(mul(instanceMatrix, float4(pos, 1.0)), 1.0);
    o_vtx.pos = worldPos.xyz;
    o_vtx.posH = mul(worldPos, g_ForwardView.view.matWorldToClip);
    o_vtx.texCoord = texCoord;
    o_vtx.normal = mul(instanceMatrix, float4(normal, 0.0));
    o_vtx.tangent.xyz = mul(instanceMatrix, float4(tangent.xyz, 0.0));
    o_vtx.tangent.w = tangent.w;
}


// Use a raw buffer on DX11 to avoid adding the StructuredBuffer flag to the instance buffer.
// On DX11, a structured buffer cannot be used as a vertex buffer, and there should be compatibility with other passes.
// On DX12, using a structured buffer results in more optimal code being generated.
#ifdef TARGET_D3D11
ByteAddressBuffer t_Instances               : REGISTER_SRV(FORWARD_BINDING_INSTANCE_BUFFER, FORWARD_SPACE_INPUT);
#else
StructuredBuffer<InstanceData> t_Instances  : REGISTER_SRV(FORWARD_BINDING_INSTANCE_BUFFER, FORWARD_SPACE_INPUT);
#endif
ByteAddressBuffer t_Vertices                : REGISTER_SRV(FORWARD_BINDING_VERTEX_BUFFER, FORWARD_SPACE_INPUT);

DECLARE_PUSH_CONSTANTS(ForwardPushConstants, g_Push, FORWARD_BINDING_PUSH_CONSTANTS, FORWARD_SPACE_INPUT);

// Version of the vertex shader that uses buffer loads to read vertex attributes and transforms.
void buffer_loads(
    in uint i_vertex : SV_VertexID,
	in uint i_instance : SV_InstanceID,
    out VSOutput o_vtx
)
{
    i_instance += g_Push.startInstanceLocation;
    i_vertex += g_Push.startVertexLocation;

#ifdef TARGET_D3D11
    const InstanceData instance = LoadInstanceData(t_Instances, i_instance * c_SizeOfInstanceData);
#else
    const InstanceData instance = t_Instances[i_instance];
#endif

    float3 pos = asfloat(t_Vertices.Load3(g_Push.positionOffset + i_vertex * c_SizeOfPosition));
    float2 texCoord = asfloat(t_Vertices.Load2(g_Push.texCoordOffset + i_vertex * c_SizeOfTexcoord));
    uint packedNormal = t_Vertices.Load(g_Push.normalOffset + i_vertex * c_SizeOfNormal);
    uint packedTangent = t_Vertices.Load(g_Push.tangentOffset + i_vertex * c_SizeOfNormal);
    float3 normal = Unpack_RGB8_SNORM(packedNormal);
    float4 tangent = Unpack_RGBA8_SNORM(packedTangent);

    o_vtx.pos = mul(instance.transform, float4(pos, 1.0)).xyz;
    o_vtx.texCoord = texCoord;
    o_vtx.normal = mul(instance.transform, float4(normal, 0)).xyz;
    o_vtx.tangent.xyz = mul(instance.transform, float4(tangent.xyz, 0)).xyz;
    o_vtx.tangent.w = tangent.w;

    float4 worldPos = float4(o_vtx.pos, 1.0);
    o_vtx.posH = mul(worldPos, g_ForwardView.view.matWorldToClip);
}

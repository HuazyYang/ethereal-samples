#include "../Common/ShaderCommon.hlsli"
#include <donut/shaders/binding_helpers.hlsli>

struct FullScreenQuadCB {
    float g_NearClipZ;
    float g_FarClipZ;
    float g_QuadZ;
};

DECLARE_PUSH_CONSTANTS(FullScreenQuadCB, g_FullScreenQuadCB, 0, 0);

void main(in uint gfsdk_VertexID : SV_VertexID, in uint gl_InstanceID : SV_InstanceID, out VxgiFullScreenQuadOutput OUT, out float4 gfsdk_Position : SV_Position)
{
    uint u = gfsdk_VertexID & 1;
    uint v = (gfsdk_VertexID >> 1) & 1;
    OUT.uv = float2(u, v);
    gfsdk_Position = float4(OUT.uv * 2 - 1, g_FullScreenQuadCB.g_QuadZ, 1);
    OUT.posProj = float4(gfsdk_Position.xy, g_FullScreenQuadCB.g_NearClipZ, 1);
    OUT.uv.y = 1.0 - OUT.uv.y;
    OUT.instanceID = float(gl_InstanceID);
}

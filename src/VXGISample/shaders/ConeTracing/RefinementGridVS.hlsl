#include "ConeTracingCommon.hlsli"

Texture2D t_RefinementGrid : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_3, 0);
void divrem(float a, float b, float invb, out float div, out float rem)
{
a = (a + 0.5) * invb;
div = floor(a);
rem = floor(frac(a) * b);
}
void main(in uint gfsdk_VertexID : SV_VertexID, in uint gl_InstanceID: SV_InstanceID, out VxgiFullScreenQuadOutput OUT, out float4 gfsdk_Position : SV_Position)
{
float vertexID = float(gfsdk_VertexID);
float quadID, vertexInQuad;
divrem(vertexID, 6.0, 1/6.0, quadID, vertexInQuad);
float quadX, quadY;
divrem(quadID, g_RefinementGridResolution.x, g_RefinementGridResolution.z, quadY, quadX);
bool quadPresent = t_RefinementGrid[int2(quadX, quadY)].x != 0;
float x = float((0x32u >> int(vertexInQuad)) & 1);
float y = float((0x2Cu >> int(vertexInQuad)) & 1);
if (!quadPresent) y = 1.0 - y;
x = (quadX + x) * TRACING_REFINEMENT_GRID_SIZE * g_GBuffer.gbufferSizeInv.x;
y = (quadY + y) * TRACING_REFINEMENT_GRID_SIZE * g_GBuffer.gbufferSizeInv.y;
y = 1.0 - y;
gfsdk_Position = float4(float2(x, y) * 2 - 1, 0, 1);
OUT.uv = float2(0, 0);
OUT.posProj = float4scalar(0);
OUT.instanceID = float(gl_InstanceID);
}

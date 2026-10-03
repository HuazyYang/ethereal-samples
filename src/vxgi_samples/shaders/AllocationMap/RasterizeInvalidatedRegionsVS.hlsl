#include "../Common/ShaderCommon.hlsli"
void main(
    in uint gfsdk_VertexID: SV_VertexID,
    in float4 in_xyBounds: XY_BOUNDS,
    in uint4 in_zbits: Z_BITS,
    out float4 gfsdk_Position: SV_Position,
    out uint4 out_zbits: Z_BITS)
{
    gfsdk_Position.x = bool(gfsdk_VertexID & 1) ? in_xyBounds.z : in_xyBounds.x;
    gfsdk_Position.y = bool(gfsdk_VertexID & 2) ? in_xyBounds.w : in_xyBounds.y;
    gfsdk_Position.z = 0.f;
    gfsdk_Position.w = 1.f;
    gfsdk_Position.y = -gfsdk_Position.y;
    out_zbits = in_zbits;
}

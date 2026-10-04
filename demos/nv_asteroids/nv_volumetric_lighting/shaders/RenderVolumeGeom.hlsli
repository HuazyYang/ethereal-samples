// RenderVolumeGeom.hlsli
//
// Interfaces between the light-volume geometry stages (RenderVolume_VS/HS/DS.hlsl) and the volume
// pixel shader (RenderVolume_PS.hlsl).
//
//   VS -> HS / PS : SV_POSITION, TEXCOORD0 = world position (w = 1),
//                   TEXCOORD1 = position in the light's clip space (frustum) or unit direction (omni)
//   HS -> DS      : TEXCOORD0 / TEXCOORD1 control points, quad tess factors (+ 4 unused floats)
//   DS -> PS      : SV_POSITION, TEXCOORD0 = world position

#ifndef NVVL_RENDERVOLUMEGEOM_HLSLI
#define NVVL_RENDERVOLUMEGEOM_HLSLI

struct VS_POLYGONAL_OUTPUT
{
    float4 vPos : SV_POSITION;
    float4 vWorldPos : TEXCOORD0;
    float4 vClipPos : TEXCOORD1;
};

struct HS_POLYGONAL_CONTROL_POINT_OUTPUT
{
    float4 vWorldPos : TEXCOORD0;
    float4 vClipPos : TEXCOORD1;
};

struct HS_POLYGONAL_CONSTANT_DATA_OUTPUT
{
    float fEdges[4] : SV_TESSFACTOR;
    float fInside[2] : SV_INSIDETESSFACTOR;
    float fReserved[4] : TEXCOORD2; // always 0, never read by the DS  // unresolved: original purpose
};

struct DS_POLYGONAL_OUTPUT
{
    float4 vPos : SV_POSITION;
    float4 vWorldPos : TEXCOORD0;
};

#endif // NVVL_RENDERVOLUMEGEOM_HLSLI

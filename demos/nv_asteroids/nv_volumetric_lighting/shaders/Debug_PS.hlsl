// Debug_PS.hlsl
//
// Wireframe debug pixel shader (DebugFlags::WIREFRAME): ContextImp_D3D11::DrawFrustumGrid / DrawFrustumBase /
// DrawFrustumCap / DrawOmniVolume bind it together with rs_Wireframe_. Front faces are drawn red,
// back faces green.
// Blob: NvVolumetricLighting.d3d11.dll 0x1800112B0 (table ps_Debug[0] @ 0x1801FE040)

struct PS_INPUT
{
    float4 vPos : SV_POSITION;
    float4 vWorldPos : TEXCOORD0;
};
float4 main(PS_INPUT input, bool bIsFrontFace : SV_ISFRONTFACE) : SV_TARGET
{
    return (bIsFrontFace) ? float4(1, 0, 0, 1) : float4(0, 1, 0, 1);
}

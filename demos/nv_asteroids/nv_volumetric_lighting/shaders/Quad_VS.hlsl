// Quad_VS.hlsl
//
// Fullscreen triangle vertex shader (ContextImp_D3D11::DrawFullscreen: Draw(3, 0), no input layout).
// Blob: NvVolumetricLighting.d3d11.dll 0x1801FAD70 (table vs_Quad[0] @ 0x1801FF2E8)

#include "ShaderCommon.hlsli"
#include "Quad.hlsli"

VS_QUAD_OUTPUT main(uint id : SV_VERTEXID)
{
    VS_QUAD_OUTPUT output;
    output.vTex = float2((id << 1) & 2, id & 2);
    output.vPos = float4(output.vTex * float2(2, -2) + float2(-1, 1), 1, 1);
    output.vWorldPos = mul(g_mViewProjInv, output.vPos);
    output.vWorldPos *= 1.0f / output.vWorldPos.w;
    return output;
}

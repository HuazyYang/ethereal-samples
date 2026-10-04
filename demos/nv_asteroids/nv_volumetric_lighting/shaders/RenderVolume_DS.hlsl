// RenderVolume_DS.hlsl
//
// Light volume domain shader. Evaluates the tessellated light-volume patch and displaces every
// vertex onto the shadow-map depth, so the rasterized volume surface is the boundary between lit
// and shadowed space as seen from the light.
//
//   VOLUMETYPE_FRUSTUM (directional / spot light, patches from DrawFrustumGrid):
//     The patch lies on the far plane of the light frustum (light clip space xy in [-1,1]).
//     For points strictly inside the frustum the shadow-map cascades are searched from the
//     last to the first; the first-most element containing the point and having depth < 1 wins.
//     The (x, y, shadow depth) point is unprojected with g_mLightProjInv[element] and pulled
//     towards the eye by g_fGodrayBias. Points on the frustum border stay on the far plane.
//   VOLUMETYPE_PARABOLOID (omni light, patches from DrawOmniVolume):
//     The patch lies on a sphere around the light. The direction is mapped onto a dual-paraboloid
//     shadow map (element 0 = front (z > 0), element 1 = back hemisphere), the sampled depth is
//     remapped linearly to [g_fLightZNear, g_fLightZFar] and the vertex placed at that distance.
//
// Caller: ContextImp_D3D11::RenderVolume_DoVolume_Directional / _Spotlight / _Omni
//         (NvVolumetricLighting.d3d11.dll 0x18000A120 / 0x18000A8D0 / 0x18000B700)
// Table: ds_RenderVolume @ 0x1801FF2F0 (16 entries,
//        index = SHADOWMAPTYPE | CASCADECOUNT << 1 | VOLUMETYPE << 3)

#include "ShaderCommon.hlsli"
#include "RenderVolumeGeom.hlsli"
#include "ShadowMap.hlsli"

#ifndef VOLUMETYPE
#define VOLUMETYPE VOLUMETYPE_FRUSTUM
#endif

// Bilinear interpolation over the quad patch: corners 0-1 along u at v = 0, 3-2 along u at v = 1
#define BILERP_PATCH(member, uv) lerp(lerp(patch[0].member, patch[1].member, uv.x), lerp(patch[3].member, patch[2].member, uv.x), uv.y)

[domain("quad")]
DS_POLYGONAL_OUTPUT main(HS_POLYGONAL_CONSTANT_DATA_OUTPUT input, float2 uv : SV_DomainLocation,
                         const OutputPatch<HS_POLYGONAL_CONTROL_POINT_OUTPUT, 4> patch)
{
    DS_POLYGONAL_OUTPUT output;
    float4 vWorldPos;

#if (VOLUMETYPE == VOLUMETYPE_FRUSTUM)
    float2 vClipPos = BILERP_PATCH(vClipPos.xy, uv);
    if (all(abs(vClipPos) < 0.99951171875f))
    {
        vWorldPos = BILERP_PATCH(vWorldPos, uv);

        // Find the shadow-map element (cascade) covering the point, last to first.
        int iElement = -1;
        float3 vShadowPos = float3(0, 0, 0);
        [unroll]
        for (int i = SHADOWMAP_ELEMENT_COUNT - 1; i >= 0; --i)
        {
            float3 vLightClip = mul(g_mLightProj[i], vWorldPos).xyw;
            vLightClip.xy *= 1.0f / vLightClip.z;
            if (all(abs(vLightClip.xy) < 1.0f))
            {
                float fDepth = SampleShadowMap(vLightClip.xy, i);
                if (fDepth < 1.0f)
                {
                    iElement = i;
                    vShadowPos = float3(vLightClip.xy, fDepth);
                }
            }
        }

        if (iElement >= 0)
        {
            // Unproject the shadowed point and bias it towards the viewer.
            float4 vShadowWorld = mul(g_mLightProjInv[iElement], float4(vShadowPos, 1));
            vShadowWorld.xyz *= 1.0f / vShadowWorld.w;
            vWorldPos = float4(lerp(g_vEyePosition, vShadowWorld.xyz, 1.0f - g_fGodrayBias), 1);
        }
    }
    else
    {
        vWorldPos = mul(g_mLightToWorld, float4(vClipPos, 1, 1));
        vWorldPos *= 1.0f / vWorldPos.w;
    }
    output.vPos = mul(g_mViewProj, vWorldPos);
#else // VOLUMETYPE_PARABOLOID
    float4 vLightPos = mul(g_mLightProj[0], BILERP_PATCH(vWorldPos, uv));
    vLightPos.xyz /= vLightPos.w;

    // Dual-paraboloid projection of the direction.
    float3 vDir = float3(vLightPos.xy, abs(vLightPos.z));
    vDir /= length(vDir);
    uint uHemisphere = (vLightPos.z > 0) ? 0 : 1;
    float2 vShadowUV = vDir.xy / (vDir.z + 1.0f);
    float fDepth = SampleShadowMap(vShadowUV, uHemisphere);
    float fDistance = lerp(g_fLightZNear, g_fLightZFar, fDepth);

    float3 vVolumeDir = normalize(BILERP_PATCH(vClipPos.xyz, uv));
    vWorldPos = mul(g_mLightProjInv[0], float4(vVolumeDir * fDistance, 1));
    vWorldPos *= 1.0f / vWorldPos.w;
    output.vPos = mul(g_mViewProj, float4(vWorldPos.xyz, 1));
#endif

    output.vWorldPos = vWorldPos;
    return output;
}

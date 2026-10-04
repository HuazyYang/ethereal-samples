// ShadowMap.hlsli
//
// Shadow-map access for the light-volume domain shader. The application's shadow map is bound to
// t1 (RenderVolumeArgs shadow map SRV); its layout is described by ShadowMapDesc and uploaded to
// cbVolume (g_vElementOffsetAndScale[], g_uElementIndex[], g_mLightProj[] / g_mLightProjInv[]).
//
//   SHADOWMAPTYPE_ATLAS : all elements (cascades / paraboloid halves) live in one 2D texture;
//                         g_vElementOffsetAndScale[i] = (offset.xy, scale.xy) in UV space.
//   SHADOWMAPTYPE_ARRAY : element i lives in array slice g_uElementIndex[i].

#ifndef NVVL_SHADOWMAP_HLSLI
#define NVVL_SHADOWMAP_HLSLI

#ifndef SHADOWMAPTYPE
#define SHADOWMAPTYPE SHADOWMAPTYPE_ATLAS
#endif

#ifndef CASCADECOUNT
#define CASCADECOUNT CASCADECOUNT_1
#endif

// Number of shadow-map elements (cascades) searched by the frustum volume.
#define SHADOWMAP_ELEMENT_COUNT (CASCADECOUNT + 1)

#if (SHADOWMAPTYPE == SHADOWMAPTYPE_ARRAY)
Texture2DArray<float> tShadowMap : register(t1);
#else
Texture2D<float> tShadowMap : register(t1);
#endif

// Sample the depth of shadow-map element 'element' at light clip-space position xy in [-1,1]
float SampleShadowMap(float2 vLightClipXY, uint element)
{
    float2 vUV = vLightClipXY * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    vUV = g_vElementOffsetAndScale[element].zw * vUV + g_vElementOffsetAndScale[element].xy;
#if (SHADOWMAPTYPE == SHADOWMAPTYPE_ARRAY)
    return tShadowMap.SampleLevel(sBilinear, float3(vUV, g_uElementIndex[element]), 0);
#else
    return tShadowMap.SampleLevel(sBilinear, vUV, 0);
#endif
}

#endif // NVVL_SHADOWMAP_HLSLI

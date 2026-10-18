// NVIDIA GVDB custom diffuse/reflected-sky surface shader, ported from RenderCustom.cu.
// Copyright 2017 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0
#if !defined(GVDB_CUSTOM_TYPED_ATLAS)
#define GVDB_ATLAS_RAW 1
#endif
#include "GVDBRaycastCommon.slang"
[shader("compute")]
[numthreads(8,8,1)]
void raycast_kernel(uint3 id : SV_DispatchThreadID)
{
    if(any(id.xy >= g_Parameters.image.xy))return;
    uint pixel=id.y*g_Parameters.image.x+id.x;
    float3 origin=g_Parameters.rayOrigin.xyz;
    float3 direction=pixelDirection(id.xy);
    float enter,leave;
    float4 color=float4(0,0,0.1,1);
    float depth=asfloat(0x7f800000u);
    if(intersectVolume(origin,direction,enter,leave))
    {
        // Preserve the legacy sampling wavefront and first sample above threshold.
        float t=g_Parameters.march.x*ceil(enter/g_Parameters.march.x);
        for(uint step=0;step<g_Parameters.image.w && t<leave;++step,t+=g_Parameters.march.x)
        {
            float3 hit=origin+direction*t;
            VDBNode leaf;uint leafIndex;
            if(!findLeaf(g_Nodes,g_Children,g_Levels,g_Parameters.volume,hit,leaf,leafIndex))continue;
            if(sampleVolume(hit)<g_Parameters.valueRange.z)continue;
            float3 gradient=float3(sampleVolume(hit-float3(.5,0,0))-sampleVolume(hit+float3(.5,0,0)),
                sampleVolume(hit-float3(0,.5,0))-sampleVolume(hit+float3(0,.5,0)),
                sampleVolume(hit-float3(0,0,.5))-sampleVolume(hit+float3(0,0,.5)));
            float3 normal=dot(gradient,gradient)>1e-12?normalize(gradient):-direction;
            float3 towardLight=g_Parameters.light.xyz-hit;
            float3 lightDirection=dot(towardLight,towardLight)>1e-12?normalize(towardLight):normal;
            float3 reflected=normalize(reflect(-direction,normal));
            float diffuse=max(0.0,dot(normal,lightDirection));
            float sky=clamp(reflected.y,0.0,1.0);
            color=float4(float3(diffuse*.6)+sky*float3(0,.3,.7),1);
            depth=t;break;
        }
    }
    g_Output[pixel]=color;g_Depth[pixel]=depth;
}

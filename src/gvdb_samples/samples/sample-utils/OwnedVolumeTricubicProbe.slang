// Independent test access to the production interpolation function.
// SPDX-License-Identifier: Apache-2.0
#include "OwnedVolumeTricubic.slang"
StructuredBuffer<float4> probes:register(t6);
RWStructuredBuffer<float4> values:register(u2);
[shader("compute")][numthreads(64,1,1)]
void probeTricubic(uint3 id:SV_DispatchThreadID){
    if(id.x>=g_Parameters.image.x)return;
    float3 p=probes[id.x].xyz,o=g_Parameters.rayOrigin.xyz;
    float3 gradient=float3(tricubicSample(p-float3(.5,0,0),o)-tricubicSample(p+float3(.5,0,0),o),
        tricubicSample(p-float3(0,.5,0),o)-tricubicSample(p+float3(0,.5,0),o),
        tricubicSample(p-float3(0,0,.5),o)-tricubicSample(p+float3(0,0,.5),o));
    values[id.x]=float4(tricubicSample(p,o),gradient);
}

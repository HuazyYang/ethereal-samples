// SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct Parameters {
    VolumeInfo volume;uint4 image;
    float4 rayOrigin,direction00,directionDx,directionDy,march,valueRange,background,light;
    uint4 counts;float4 sampling;float4 options;
};
ConstantBuffer<Parameters> g_Parameters:register(b0);
ByteAddressBuffer g_Atlas:register(t0);
ByteAddressBuffer g_Nodes:register(t1);
ByteAddressBuffer g_Children:register(t2);
StructuredBuffer<LevelInfo> g_Levels:register(t3);
ByteAddressBuffer g_Color:register(t5);
RWStructuredBuffer<float4> g_Output:register(u0);
RWStructuredBuffer<float> g_Depth:register(u1);
#define GVDB_RAY_QUERY_RAW 1
#define GVDB_RAY_QUERY_BORDER_OPTION 1
#include "GVDBRayQueryTrace.slang"
#include "GVDBTricubicTrace.slang"
[shader("compute")][numthreads(8,8,1)]
void renderTricubic(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=g_Parameters.image.xy))return;
    uint pixel=id.y*g_Parameters.image.x+id.x;
    float3 d=g_Parameters.direction00.xyz+(float(id.x)+.5)*g_Parameters.directionDx.xyz+(float(id.y)+.5)*g_Parameters.directionDy.xyz;
    float length2=dot(d,d);d=length2>0&&isfinite(length2)?d*rsqrt(length2):float3(0);
    TraceResult hit=traceTricubic(g_Parameters.rayOrigin.xyz,d);
    float4 color=g_Parameters.background;float depth=asfloat(0x7f800000u);
    if(hit.hit.w>0){
        float3 light=g_Parameters.light.xyz-hit.hit.xyz;float ll=dot(light,light);light=ll>0&&isfinite(ll)?light*rsqrt(ll):float3(0);
        float diffuse=.9*max(0.0,dot(hit.normal.xyz,light));
        if(g_Parameters.options.x>0){TraceResult shadow=traceRay(hit.hit.xyz+hit.normal.xyz*g_Parameters.options.y,light);
            if(shadow.hit.w>0)diffuse*=1-g_Parameters.options.x;
            if(shadow.hit.w<0)hit.hit.w=-1;}
        float3 base=1;
        if(g_Parameters.options.z!=0){
            int3 cell=int3(floor(hit.atlas));uint3 resolution=g_Parameters.volume.atlasResolutionAndApron.xyz;
            bool border=g_Parameters.options.w!=0&&(any(cell<0)||any(cell>=int3(resolution)));
            uint3 p=uint3(clamp(cell,int3(0),int3(resolution)-1));
            uint rgba=g_Color.Load(flatten3(p,resolution)*4);
            base=border?float3(0):float3(rgba&255,(rgba>>8)&255,(rgba>>16)&255)/255.0;
        }
        color=float4(base*(diffuse+.1),1);depth=dot(hit.hit.xyz-g_Parameters.rayOrigin.xyz,d);
    }
    if(hit.hit.w<0)depth=-1;
    g_Output[pixel]=color;g_Depth[pixel]=depth;
}

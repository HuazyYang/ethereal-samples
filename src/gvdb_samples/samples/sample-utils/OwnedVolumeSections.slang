// SPDX-License-Identifier: Apache-2.0
#include "../../gvdb/slang/GVDBTypes.slang"
struct Parameters {
    VolumeInfo volume;
    uint4 image;
    float4 rayOrigin, direction00, directionDx, directionDy;
    float4 march, valueRange, background, light;
    uint4 counts;
    float4 sampling, point, normal;
    uint4 options; // mode, color enabled, active leaf count, reserved
};
ConstantBuffer<Parameters> g_Parameters : register(b0);
ByteAddressBuffer g_Atlas : register(t0);
ByteAddressBuffer g_Nodes : register(t1);
ByteAddressBuffer g_Children : register(t2);
StructuredBuffer<LevelInfo> g_Levels : register(t3);
StructuredBuffer<float4> transferTable : register(t4);
ByteAddressBuffer colorAtlas : register(t5);
RWStructuredBuffer<float4> outputColor : register(u0);
RWStructuredBuffer<float> outputDepth : register(u1);
#define GVDB_RAY_QUERY_RAW 1
#include "../../gvdb/slang/GVDBRayQueryTrace.slang"
float4 sectionTransfer(float value) {
    float coordinate = saturate((value-g_Parameters.valueRange.z)/
        (g_Parameters.valueRange.y-g_Parameters.valueRange.x));
    return transferTable[uint(coordinate*16300.0)];
}
float4 sectionColor(float3 atlas) {
    if (g_Parameters.options.y == 0) return 1;
    uint3 p = uint3(clamp(int3(atlas),int3(0),int3(g_Parameters.volume.atlasResolutionAndApron.xyz)-1));
    uint c = colorAtlas.Load(flatten3(p,g_Parameters.volume.atlasResolutionAndApron.xyz)*4);
    return float4(c&255,(c>>8)&255,(c>>16)&255,c>>24)/255.0;
}
float3 sectionLight(float3 position) {
    float3 v=g_Parameters.light.xyz-position;
    float lengthSquared=dot(v,v);
    return lengthSquared>0 && isfinite(lengthSquared) ? v*rsqrt(lengthSquared) : float3(0);
}
[shader("compute")][numthreads(8,8,1)]
void renderSection(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy>=g_Parameters.image.xy)) return;
    uint index=id.y*g_Parameters.image.x+id.x;
    float4 result=g_Parameters.background;
    float depth=asfloat(0x7f800000u);
    if (g_Parameters.options.x==0) {
        result=0;
        float3 p=g_Parameters.point.xyz+float3(float(id.x)*2/g_Parameters.image.x-1,0,
            float(id.y)*2/g_Parameters.image.y-1)*g_Parameters.normal.xyz;
        VDBNode leaf=(VDBNode)0; uint leafIndex=0;
        if (g_Parameters.options.z!=0 && findLeaf(g_Nodes,g_Children,g_Levels,g_Parameters.volume,p,leaf,leafIndex)) {
            float3 atlas=float3(leaf.atlasPosition)+p-float3(leaf.position);
            float4 section=sectionTransfer(atlasSample(atlas));
            result.xyz=section.xyz*section.w;
            depth=0;
        }
    } else if (g_Parameters.options.z!=0) {
        float3 origin=g_Parameters.rayOrigin.xyz;
        float3 direction=g_Parameters.direction00.xyz+(float(id.x)+.5)*g_Parameters.directionDx.xyz+
            (float(id.y)+.5)*g_Parameters.directionDy.xyz;
        float lengthSquared=dot(direction,direction);
        direction=lengthSquared>0 && isfinite(lengthSquared)?direction*rsqrt(lengthSquared):float3(0);
        float denominator=dot(direction,g_Parameters.normal.xyz);
        float plane=denominator!=0 ? dot(g_Parameters.point.xyz-origin,g_Parameters.normal.xyz)/denominator : -1;
        float3 start=origin;
        float scalar=0;
        float4 section=float4(1,1,1,0);
        if (plane>0 && isfinite(plane)) {
            start=origin+plane*direction;
            VDBNode leaf=(VDBNode)0; uint leafIndex=0;
            if (findLeaf(g_Nodes,g_Children,g_Levels,g_Parameters.volume,start,leaf,leafIndex)) {
                float3 atlas=float3(leaf.atlasPosition)+start-float3(leaf.position);
                scalar=atlasSample(atlas);
                section=sectionTransfer(scalar);
                section.xyz*=sectionColor(atlas).xyz;
            }
        }
        TraceResult hit=traceRay(start,direction);
        if (hit.hit.w<0) depth=-1;
        else if (hit.hit.w>0) {
            float diffuse=scalar>g_Parameters.valueRange.z?1:.8*max(0,dot(hit.normal.xyz,sectionLight(hit.hit.xyz)));
            result=lerp(sectionColor(hit.atlas)*diffuse,section,section.w);
            depth=length(hit.hit.xyz-origin);
        } else {
            result=lerp(g_Parameters.background,section,section.w);
            if (section.w>0) depth=plane;
        }
    }
    result.w=1;
    outputColor[index]=float4(uint4(saturate(result)*255.0))/255.0;
    outputDepth[index]=depth;
}

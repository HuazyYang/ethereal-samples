// Deterministic counterpart of PointFusionScan.cu.
// Copyright 2018 NVIDIA Corporation. SPDX-License-Identifier: Apache-2.0
struct ScanBox {float4 minimum,maximum;uint4 color;};
struct ScanParameters {uint4 counts;float4 originAndMaximum,direction00,directionDx,directionDy,cityMinimum,cityMaximum;uint4 options;};
ConstantBuffer<ScanParameters> g_Parameters:register(b0);
StructuredBuffer<ScanBox> g_Boxes:register(t0);
StructuredBuffer<float4> g_Positions:register(t1);
StructuredBuffer<uint> g_Colors:register(t2);
StructuredBuffer<uint> g_Valid:register(t3);
StructuredBuffer<uint> g_Offsets:register(t4);
RWStructuredBuffer<float4> g_OutputPositions:register(u0);
RWStructuredBuffer<uint> g_OutputColors:register(u1);
RWStructuredBuffer<uint> g_OutputIndices:register(u2);
RWStructuredBuffer<uint> g_State:register(u3);
float random(inout uint state){state=1664525u*state+1013904223u;return float(state&0x00ffffffu)*(1.0/16777216.0);}
float3 slab(float origin,float direction,float minimum,float maximum) {
    bool parallel=direction==0;
    float denominator=parallel?1:direction;
    float a=(minimum-origin)/denominator,b=(maximum-origin)/denominator;
    return float3(parallel?0:min(a,b),parallel?g_Parameters.originAndMaximum.w:max(a,b),
        parallel&&(origin<minimum||origin>maximum)?0:1);
}
bool boxHit(float3 origin,float3 direction,ScanBox box,out float distance) {
    float3 x=slab(origin.x,direction.x,box.minimum.x,box.maximum.x);
    float3 y=slab(origin.y,direction.y,box.minimum.y,box.maximum.y);
    float3 z=slab(origin.z,direction.z,box.minimum.z,box.maximum.z);
    float low=max(0,max(x.x,max(y.x,z.x))),high=min(x.y,min(y.y,z.y));
    distance=low;return x.z!=0&&y.z!=0&&z.z!=0&&high>=low&&low<g_Parameters.originAndMaximum.w;
}
[shader("compute")][numthreads(8,8,1)]
void scanBuildings(uint3 id:SV_DispatchThreadID) {
    [branch] if(any(id.xy>=g_Parameters.counts.xy))return;
    uint pixel=id.y*g_Parameters.counts.x+id.x;
    uint state=g_State[pixel];float u=random(state),v=random(state);random(state);g_State[pixel]=state;
    g_OutputPositions[pixel]=float4(0);g_OutputColors[pixel]=0;g_OutputIndices[pixel]=0;
    float3 origin=g_Parameters.originAndMaximum.xyz;
    [branch] if(any(origin.xz<g_Parameters.cityMinimum.xz)||any(origin.xz>=g_Parameters.cityMaximum.xz))return;
    float2 jitter=g_Parameters.options.x!=0?float2(u,v):float2(.5);
    float2 uv=(float2(id.xy)+jitter)/float2(g_Parameters.counts.xy);
    float3 direction=g_Parameters.direction00.xyz+uv.x*g_Parameters.directionDx.xyz+uv.y*g_Parameters.directionDy.xyz;
    float lengthSquared=dot(direction,direction);
    [branch] if(lengthSquared<=0||(asuint(lengthSquared)&0x7fffffffu)>=0x7f800000u)return;
    direction*=rsqrt(lengthSquared);
    float nearest=g_Parameters.originAndMaximum.w;uint color=0;bool hit=false;
    [loop] for(uint i=0;i<g_Parameters.counts.z;++i){float t;ScanBox box=g_Boxes[i];if(boxHit(origin,direction,box,t)&&t<nearest){nearest=t;color=box.color.x;hit=true;}}
    [branch] if(!hit)return;
    g_OutputPositions[pixel]=float4(origin+nearest*direction,1);g_OutputColors[pixel]=color;g_OutputIndices[pixel]=1;
}
[shader("compute")][numthreads(256,1,1)]
void compactScanHits(uint3 id:SV_DispatchThreadID) {
    uint pixel=id.x;[branch] if(pixel>=g_Parameters.counts.w)return;
    uint valid=g_Valid[pixel],offset=g_Offsets[pixel];
    if(pixel+1==g_Parameters.counts.w)g_State[0]=offset+valid;
    [branch] if(valid==0)return;
    g_OutputPositions[offset]=g_Positions[pixel];g_OutputColors[offset]=g_Colors[pixel];g_OutputIndices[offset]=pixel;
}

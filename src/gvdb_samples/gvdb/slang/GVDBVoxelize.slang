// Binned conservative triangle and parity solid voxelization.
// Copyright 2017 NVIDIA Corporation. SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct Triangle {float4 a,b,c;};
struct VoxelizeParameters {float4 boundsMinimum,boundsMaximum;uint4 resolution;float4 values;uint4 counts;float4 transformRows[3];};
ConstantBuffer<VoxelizeParameters> g_Parameters:register(b0);
StructuredBuffer<float4> g_Vertices:register(t0);
StructuredBuffer<uint> g_Indices:register(t1);
StructuredBuffer<uint> g_BinCounts:register(t2);
StructuredBuffer<uint> g_BinOffsets:register(t3);
StructuredBuffer<Triangle> g_Triangles:register(t4);
RWStructuredBuffer<uint> g_OutputCounts:register(u0);
RWStructuredBuffer<Triangle> g_OutputTriangles:register(u1);
RWByteAddressBuffer g_Output:register(u2);
uint invocation(uint3 id){return id.x+id.y*(65535u*256u);}
float4 transformVertex(float3 vertex){float4 v=float4(vertex,1);return float4(dot(g_Parameters.transformRows[0],v),dot(g_Parameters.transformRows[1],v),dot(g_Parameters.transformRows[2],v),1);}
bool loadTriangle(uint index,out Triangle triangle,out int first,out int last) {
    uint3 face=uint3(g_Indices[index*3],g_Indices[index*3+1],g_Indices[index*3+2]);
    triangle.a=triangle.b=triangle.c=float4(0);first=0;last=-1;
    if(any(face>=uint3(g_Parameters.counts.x)))return false;
    triangle.a=transformVertex(g_Vertices[face.x].xyz);triangle.b=transformVertex(g_Vertices[face.y].xyz);triangle.c=transformVertex(g_Vertices[face.z].xyz);
    float low=min(triangle.a.y,min(triangle.b.y,triangle.c.y));
    float high=max(triangle.a.y,max(triangle.b.y,triangle.c.y));
    float halfVoxel=.5*(g_Parameters.boundsMaximum.y-g_Parameters.boundsMinimum.y)/float(g_Parameters.resolution.y);
    float firstFloat=floor((low-halfVoxel-g_Parameters.boundsMinimum.y)/g_Parameters.values.w);
    float lastFloat=floor((high+halfVoxel-g_Parameters.boundsMinimum.y)/g_Parameters.values.w);
    if(firstFloat>=float(g_Parameters.counts.z)||lastFloat<0)return false;
    first=int(max(0.0,firstFloat));last=int(min(float(g_Parameters.counts.z-1),lastFloat));return first<=last;
}
[shader("compute")][numthreads(256,1,1)]
void clearTriangleBins(uint3 id:SV_DispatchThreadID){uint i=invocation(id);if(i<g_Parameters.counts.z)g_OutputCounts[i]=0;}
[shader("compute")][numthreads(256,1,1)]
void countTriangleBins(uint3 id:SV_DispatchThreadID){
    uint i=invocation(id);[branch] if(i>=g_Parameters.counts.y)return;
    Triangle triangle;int first,last;bool valid=loadTriangle(i,triangle,first,last);
    [branch] if(!valid)return;
    for(int bin=first;bin<=last;++bin)InterlockedAdd(g_OutputCounts[bin],1);
}
[shader("compute")][numthreads(256,1,1)]
void scatterTriangleBins(uint3 id:SV_DispatchThreadID){
    uint i=invocation(id);[branch] if(i>=g_Parameters.counts.y)return;
    Triangle triangle;int first,last;bool valid=loadTriangle(i,triangle,first,last);
    [branch] if(!valid)return;
    for(int bin=first;bin<=last;++bin){uint offset;InterlockedAdd(g_OutputCounts[bin],1,offset);g_OutputTriangles[g_BinOffsets[bin]+offset]=triangle;}
}
bool axisSeparates(float3 axis,float3 a,float3 b,float3 c){
    float x=dot(axis,a),y=dot(axis,b),z=dot(axis,c),radius=.5*dot(abs(axis),float3(1));
    return min(x,min(y,z))>radius||max(x,max(y,z))< -radius;
}
bool touchesVoxel(float3 a,float3 b,float3 c){
    if(any(min(a,min(b,c))>.5)||any(max(a,max(b,c))< -.5))return false;
    float3 e0=b-a,e1=c-b,e2=a-c;
    if(axisSeparates(cross(e0,e1),a,b,c))return false;
    [loop] for(uint edge=0;edge<3;++edge){
        float3 e=edge==0?e0:(edge==1?e1:e2);
        if(axisSeparates(float3(0,e.z,-e.y),a,b,c)||axisSeparates(float3(-e.z,0,e.x),a,b,c)||axisSeparates(float3(e.y,-e.x,0),a,b,c))return false;
    }
    return true;
}
float separateMultiply(float a,float b) {
    __target_switch {
    case cuda: __intrinsic_asm "__fmul_rn($0, $1)";
    default: precise float value=a*b;return value;
    }
}
float separateAdd(float a,float b) {
    __target_switch {
    case cuda: __intrinsic_asm "__fadd_rn($0, $1)";
    default: precise float value=a+b;return value;
    }
}
bool crossesPositiveX(float3 a,float3 b,float3 c){
    // Preserve the oriented top-left shared-edge rule. 'precise' prevents FMA
    // contraction; hardware targets use their native IEEE rounding mode.
    float u=separateAdd(separateMultiply(c.y,b.z),-separateMultiply(c.z,b.y));
    float v=separateAdd(separateMultiply(a.y,c.z),-separateMultiply(a.z,c.y));
    float w=separateAdd(separateMultiply(b.y,a.z),-separateMultiply(b.z,a.y));
    if(!((u>=0&&v>=0&&w>=0)||(u<=0&&v<=0&&w<=0)))return false;
    float numerator=separateAdd(separateAdd(separateMultiply(u,a.x),separateMultiply(v,b.x)),separateMultiply(w,c.x));
    float determinant=separateAdd(separateAdd(u,v),w);
    if(numerator*determinant<=0)return false;
    if(u!=0&&v!=0&&w!=0)return true;
    float2 edge=u==0?c.zy-b.zy:(v==0?a.zy-c.zy:b.zy-a.zy);
    float order=2*sign(edge.x)+sign(edge.y);
    return determinant*order>0;
}
float voxelValue(uint index){
    uint3 voxel=unflatten3(index,g_Parameters.resolution.xyz);
    float3 width=(g_Parameters.boundsMaximum.xyz-g_Parameters.boundsMinimum.xyz)/float3(g_Parameters.resolution.xyz);
    float3 center=g_Parameters.boundsMinimum.xyz+(float3(voxel)+.5)*width;
    uint bin=min(uint(max(0.0,floor((center.y-g_Parameters.boundsMinimum.y)/g_Parameters.values.w))),g_Parameters.counts.z-1);
    uint first=g_BinOffsets[bin],end=first+g_BinCounts[bin],crossings=0;
    [loop] for(uint i=first;i<end;++i){
        Triangle triangle=g_Triangles[i];
        float3 a=(triangle.a.xyz-center)/width,b=(triangle.b.xyz-center)/width,c=(triangle.c.xyz-center)/width;
        a.y+=a.y==floor(a.y)?1.0/64.0:0;b.y+=b.y==floor(b.y)?1.0/64.0:0;c.y+=c.y==floor(c.y)?1.0/64.0:0;
        if(min(a.y,min(b.y,c.y))>.5||max(a.y,max(b.y,c.y))< -.5||min(a.z,min(b.z,c.z))>.5||max(a.z,max(b.z,c.z))< -.5||max(a.x,max(b.x,c.x))< -.5)continue;
        if(crossesPositiveX(a,b,c))++crossings;
        if(touchesVoxel(a,b,c))return g_Parameters.values.x;
    }
    return (crossings&1u)!=0?g_Parameters.values.y:g_Parameters.values.z;
}
[shader("compute")][numthreads(256,1,1)]
void solidVoxelize(uint3 id:SV_DispatchThreadID){
    uint i=invocation(id),count=g_Parameters.resolution.w;
    if(g_Parameters.counts.w==0){
        [branch] if(i>=count/4+uint(count%4!=0))return;
        uint packed=0;[loop] for(uint lane=0;lane<4;++lane)if(i*4+lane<count)packed|=(uint(voxelValue(i*4+lane))&255u)<<(lane*8);
        g_Output.Store(i*4,packed);
    }else{
        [branch] if(i>=count)return;
        float value=voxelValue(i);g_Output.Store(i*4,g_Parameters.counts.w==3?asuint(value):uint(int(value)));
    }
}

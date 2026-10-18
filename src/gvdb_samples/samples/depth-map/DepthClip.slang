// SPDX-License-Identifier: Apache-2.0
// Dedicated compatibility marcher for the depth-map sample. No legacy CUDA ABI.
#include "GVDBTypes.slang"
struct DepthParameters {
    VolumeInfo volume;
    uint4 image; // width, height, maximum brick steps, exact-depth mode
    float4 origin;
    float4 direction00;
    float4 directionDx;
    float4 directionDy;
    float4 indexToWorldRows[3];
    float4 cameraForward;
    float4 nearFarStepBias; // near, far, index-space step, exact-mode index-space bias
    float4 integration; // extinction magnitude, albedo, density cutoff, transmission cutoff
    float4 background;
};
ConstantBuffer<DepthParameters> g_Parameters : register(b0);
StructuredBuffer<float> g_Atlas : register(t0);
ByteAddressBuffer g_Nodes : register(t1);
ByteAddressBuffer g_Children : register(t2);
StructuredBuffer<LevelInfo> g_Levels : register(t3);
StructuredBuffer<float4> g_Transfer : register(t4);
Texture2D<float> g_RasterDepth : register(t5);
RWTexture2D<uint> g_Output : register(u0);
float atlasValue(int3 coordinate) {
    uint3 res=g_Parameters.volume.atlasResolutionAndApron.xyz;
    return g_Atlas[flatten3(uint3(clamp(coordinate,int3(0),int3(res)-1)),res)];
}
float sampleLeaf(VDBNode leaf,float3 position) {
    float3 coordinate=float3(leaf.atlasPosition)+position-float3(leaf.position)-.5;
    int3 lo=int3(floor(coordinate));float3 w=coordinate-float3(lo);
    float a=lerp(atlasValue(lo),atlasValue(lo+int3(1,0,0)),w.x);
    float b=lerp(atlasValue(lo+int3(0,1,0)),atlasValue(lo+int3(1,1,0)),w.x);
    float c=lerp(atlasValue(lo+int3(0,0,1)),atlasValue(lo+int3(1,0,1)),w.x);
    float d=lerp(atlasValue(lo+int3(0,1,1)),atlasValue(lo+int3(1,1,1)),w.x);
    return lerp(lerp(a,b,w.y),lerp(c,d,w.y),w.z);
}
bool intersectBox(float3 origin,float3 direction,float3 lower,float3 upper,out float enter,out float leave) {
    enter=0;leave=asfloat(0x7f800000u);
    for(uint axis=0;axis<3;++axis) {
        if(direction[axis]==0){if(origin[axis]<lower[axis]||origin[axis]>=upper[axis])return false;}
        else {float a=(lower[axis]-origin[axis])/direction[axis],b=(upper[axis]-origin[axis])/direction[axis];
            enter=max(enter,min(a,b));leave=min(leave,max(a,b));}
    }
    return enter<leave;
}
[shader("compute")]
[numthreads(8,8,1)]
void clipVolume(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=g_Parameters.image.xy))return;
    float3 origin=g_Parameters.origin.xyz;
    float3 direction=normalize(g_Parameters.direction00.xyz+(float(id.x)+.5)*g_Parameters.directionDx.xyz+
        (float(id.y)+.5)*g_Parameters.directionDy.xyz);
    float z=g_RasterDepth.Load(int3(id.xy,0));
    float n=g_Parameters.nearFarStepBias.x,f=g_Parameters.nearFarStepBias.y;
    float linearDepth=n*f/(f-z*(f-n));
    float3 worldRay=direction.x*g_Parameters.indexToWorldRows[0].xyz+
        direction.y*g_Parameters.indexToWorldRows[1].xyz+direction.z*g_Parameters.indexToWorldRows[2].xyz;
    float limit=linearDepth/(g_Parameters.image.w!=0?dot(worldRay,g_Parameters.cameraForward.xyz):length(worldRay));
    if(g_Parameters.image.w!=0)limit=max(0.0,limit-g_Parameters.nearFarStepBias.w);
    float enter,leave,transmission=1;float3 accumulated=float3(0);
    if(intersectBox(origin,direction,g_Parameters.volume.boundsMin.xyz,g_Parameters.volume.boundsMax.xyz,enter,leave)) {
        float t=enter;float stepSize=g_Parameters.nearFarStepBias.z;
        float3 brickSize=float3(g_Levels[0].nodeExtent.xyz);
        // Leaf-cell DDA skips empty atlas bricks and restarts on the same global sampling wavefront.
        for(uint brick=0;brick<g_Parameters.image.z&&t<leave&&t<=limit;++brick) {
            float probe=t+.001;
            float3 position=origin+direction*probe;
            float3 cell=floor(position/brickSize)*brickSize;
            float cellExit=leave;
            for(uint axis=0;axis<3;++axis)if(direction[axis]!=0) {
                float edge=direction[axis]>0?cell[axis]+brickSize[axis]:cell[axis];
                cellExit=min(cellExit,(edge-origin[axis])/direction[axis]);
            }
            VDBNode leaf;uint leafIndex;
            if(findLeaf(g_Nodes,g_Children,g_Levels,g_Parameters.volume,position,leaf,leafIndex)) {
                float sampleT=stepSize*ceil(probe/stepSize);
                for(uint sample=0;sample<256&&sampleT<cellExit&&sampleT<=limit&&transmission>g_Parameters.integration.w;++sample) {
                    float density=sampleLeaf(leaf,origin+direction*sampleT);
                    if(density>=g_Parameters.integration.z) {
                        uint index=uint(saturate((density-.1)/.5)*16300.0);
                        float4 value=g_Transfer[index];
                        float attenuation=exp(-g_Parameters.integration.x*value.w*stepSize);
                        accumulated+=value.xyz*transmission*(1-attenuation)*g_Parameters.integration.y;
                        transmission*=attenuation;
                    }
                    sampleT+=stepSize;
                }
            }
            if(transmission<=g_Parameters.integration.w)break;
            t=max(cellExit,t+.001);
        }
    }
    float alpha=1-transmission;
    float3 color=lerp(g_Parameters.background.xyz,min(accumulated,float3(1)),alpha);
    uint4 bytes=uint4(saturate(float4(color,alpha))*255.0);
    g_Output[id.xy]=bytes.x|(bytes.y<<8)|(bytes.z<<16)|(bytes.w<<24);
}

// Portable byte-exact channel operators; no texture format or byte atomics needed.
// Copyright 2017 NVIDIA Corporation. SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct RawOperatorParameters { VolumeInfo volume; float4 value; uint4 channel; };
ConstantBuffer<RawOperatorParameters> g_Parameters : register(b0);
ByteAddressBuffer g_Source : register(t0);
StructuredBuffer<int4> g_AtlasMap : register(t1);
ByteAddressBuffer g_Nodes : register(t2);
ByteAddressBuffer g_Children : register(t3);
StructuredBuffer<LevelInfo> g_Levels : register(t4);
RWByteAddressBuffer g_Destination : register(u0);

uint byteValue(float value) { return uint(int(value)) & 255u; }
float4 loadVoxel(uint index) {
    uint format = g_Parameters.channel.x;
    uint address = index * g_Parameters.channel.y;
    if (format == 0) return float4(float((g_Source.Load(address & ~3u) >> ((address & 3u)*8)) & 255u),0,0,0);
    if (format == 1) {
        uint packed = g_Source.Load(address);
        return float4(float(packed & 255u),float((packed >> 8) & 255u),float((packed >> 16) & 255u),float(packed >> 24));
    }
    if (format == 2) return float4(asfloat(g_Source.Load(address)),0,0,0);
    if (format == 3) return float4(asfloat(g_Source.Load3(address)),0);
    return asfloat(g_Source.Load4(address));
}
void storeVoxel(uint index,float4 value) {
    uint format = g_Parameters.channel.x;
    uint address = index * g_Parameters.channel.y;
    if (format == 1) {
        uint packed = byteValue(value.x) | (byteValue(value.y)<<8) | (byteValue(value.z)<<16) | (byteValue(value.w)<<24);
        g_Destination.Store(address,packed);
    } else if (format == 2) g_Destination.Store(address,asuint(value.x));
    else if (format == 3) g_Destination.Store3(address,asuint(value.xyz));
    else g_Destination.Store4(address,asuint(value));
}
uint indexOf(uint3 voxel) { return flatten3(voxel,g_Parameters.volume.atlasResolutionAndApron.xyz); }
float4 neighbor(int3 voxel) {
    uint3 resolution = g_Parameters.volume.atlasResolutionAndApron.xyz;
    return loadVoxel(indexOf(uint3(clamp(voxel,int3(0),int3(resolution)-1))));
}
// The operation number is specialized by each entry point and removed by the compiler.
float4 evaluateVoxel(uint index,uint operation) {
    uint3 voxel = unflatten3(index,g_Parameters.volume.atlasResolutionAndApron.xyz);
    uint format = g_Parameters.channel.x;
    if (operation == 0) return float4(0);
    if (operation == 1) {
        float4 value = float4(g_Parameters.value.xyz,1);
        if (format == 1) value *= 255;
        if (format == 2 && g_Parameters.value.z < 0) {
            float3 waves = sin(float3(voxel)*(12.0/(3.141592*30.0)));
            value.x = waves.x+waves.y+waves.z;
        }
        return value;
    }
    float4 value = loadVoxel(index);
    bool interior = isAtlasInterior(g_Parameters.volume,voxel);
    if (operation == 2 && !interior) {
        float3 world;
        if (atlasToWorld(g_AtlasMap,g_Parameters.volume,voxel,world)) {
            value = float4(g_Parameters.value.x);
            VDBNode leaf; uint leafIndex;
            if (findLeaf(g_Nodes,g_Children,g_Levels,g_Parameters.volume,world,leaf,leafIndex)) {
                uint3 adjacent = uint3(float3(leaf.atlasPosition)+world-float3(leaf.position));
                value = loadVoxel(indexOf(adjacent));
            }
        }
    }
    if (interior && operation >= 3) {
        int3 p = int3(voxel);
        float4 xm=neighbor(p+int3(-1,0,0)),xp=neighbor(p+int3(1,0,0));
        float4 ym=neighbor(p+int3(0,-1,0)),yp=neighbor(p+int3(0,1,0));
        float4 zm=neighbor(p+int3(0,0,-1)),zp=neighbor(p+int3(0,0,1));
        if (operation == 3) value=(g_Parameters.value.x*value+xm+xp+ym+yp+zm+zp)/(g_Parameters.value.x+6)+g_Parameters.value.y;
        if (operation == 4 && value.x == 0) {
            float sought = float(byteValue(g_Parameters.value.x));
            if (xm.x==sought || xp.x==sought || ym.x==sought || yp.x==sought || zm.x==sought || zp.x==sought)
                value.x=g_Parameters.value.y;
        }
        if (operation == 5) {
            int3 color = int3(value.xyz*g_Parameters.value.x);
            color += int3(xm.xyz*g_Parameters.value.y); color += int3(xp.xyz*g_Parameters.value.y);
            color += int3(ym.xyz*g_Parameters.value.y); color += int3(yp.xyz*g_Parameters.value.y);
            color += int3(zm.xyz*g_Parameters.value.y); color += int3(zp.xyz*g_Parameters.value.y);
            int maximum=max(color.x,max(color.y,color.z));
            if (maximum>255) color=color*255/maximum;
            value=float4(float3(color),1); // Legacy byte alpha is one, not 255.
        }
    }
    return value;
}
void dispatchVoxel(uint invocation,uint operation) {
    if (invocation >= g_Parameters.channel.w) return;
    if (g_Parameters.channel.x == 0) {
        // Exactly one writer owns each 32-bit word, including partial row ends.
        uint packed=0;
        [unroll] for (uint lane=0;lane<4;++lane) {
            uint index=invocation*4+lane;
            if (index<g_Parameters.channel.z) packed |= byteValue(evaluateVoxel(index,operation).x) << (lane*8);
        }
        g_Destination.Store(invocation*4,packed);
    } else storeVoxel(invocation,evaluateVoxel(invocation,operation));
}
[shader("compute")][numthreads(256,1,1)]
void clearRawAtlas(uint3 id:SV_DispatchThreadID) { dispatchVoxel(id.x+id.y*(65535u*256u),0); }
[shader("compute")][numthreads(256,1,1)]
void fillRawAtlas(uint3 id:SV_DispatchThreadID) { dispatchVoxel(id.x+id.y*(65535u*256u),1); }
[shader("compute")][numthreads(256,1,1)]
void updateRawApron(uint3 id:SV_DispatchThreadID) { dispatchVoxel(id.x+id.y*(65535u*256u),2); }
[shader("compute")][numthreads(256,1,1)]
void smoothRawAtlas(uint3 id:SV_DispatchThreadID) { dispatchVoxel(id.x+id.y*(65535u*256u),3); }
[shader("compute")][numthreads(256,1,1)]
void expandByteAtlas(uint3 id:SV_DispatchThreadID) { dispatchVoxel(id.x+id.y*(65535u*256u),4); }
[shader("compute")][numthreads(256,1,1)]
void expandColorAtlas(uint3 id:SV_DispatchThreadID) { dispatchVoxel(id.x+id.y*(65535u*256u),5); }

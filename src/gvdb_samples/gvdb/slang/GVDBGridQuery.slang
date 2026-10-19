// SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
cbuffer Parameters : register(b0) {
    uint4 atlas;
    uint4 work; // Point count, position stride, mode, active leaf count.
    uint4 hierarchy;
};
ByteAddressBuffer nodes : register(t0);
ByteAddressBuffer children : register(t1);
ByteAddressBuffer values : register(t2);
StructuredBuffer<LevelInfo> levels : register(t3);
ByteAddressBuffer positions : register(t4);
RWStructuredBuffer<float4> results : register(u0);
bool queryLeaf(int3 position,out VDBNode leaf) {
    leaf=(VDBNode)0;
    uint level=hierarchy.x,index=hierarchy.y;
    if(work.w==0 || level>=hierarchy.z || hierarchy.z>10) return false;
    for(uint depth=0;depth<10;++depth) {
        LevelInfo info=levels[level];
        if(index>=info.nodes.z) return false;
        VDBNode node=loadNode(nodes,info.nodes.x+index*info.nodes.y);
        if(any(position<node.position)) return false;
        // Unsigned subtraction avoids signed overflow for distant valid points.
        uint3 relative=uint3(position)-uint3(node.position);
        if(any(relative>=uint3(info.nodeExtent.xyz))) return false;
        if(level==0) {leaf=node;return ((node.metadata>>8)&255u)!=0;}
        uint2 list=node.childList;
        if(nullReference(list) || !referenceFitsIndex(list)) return false;
        uint listLevel=referenceLevel(list);
        if(listLevel>=hierarchy.z) return false;
        LevelInfo childInfo=levels[listLevel];
        uint listIndex=referenceIndex(list);
        if(listIndex>=childInfo.children.z) return false;
        uint3 childPosition=relative/uint3(levels[level-1].nodeExtent.xyz);
        if(any(childPosition>=info.children.w)) return false;
        uint2 child=children.Load2(childInfo.children.x+listIndex*childInfo.children.y+
            flatten3(childPosition,uint3(info.children.w))*8);
        if(nullReference(child) || !referenceFitsIndex(child) || referenceLevel(child)!=level-1) return false;
        index=referenceIndex(child);--level;
    }
    return false;
}
float sampleAtlas(int3 p) {
    if(any(p<0) || any(uint3(p)>=atlas.xyz)) return 0;
    return asfloat(values.Load(flatten3(uint3(p),atlas.xyz)*4));
}
[shader("compute")]
[numthreads(256,1,1)]
void queryScalarGrid(uint3 group:SV_GroupID,uint local:SV_GroupIndex) {
    uint i=(group.y*65535u+group.x)*256u+local;
    if(i>=work.x) return;
    float3 p=asfloat(positions.Load3(i*work.y));
    float4 result=0;
    // Float-to-int conversion is defined only in this representable domain.
    if(all(p>=-2147483648.0) && all(p<2147483648.0)) {
        float3 floored=floor(p);
        int3 voxel=int3(floored);
        VDBNode leaf;
        if(queryLeaf(voxel,leaf)) {
            int3 cell=voxel-leaf.position;
            if(work.z==0) result=float4(sampleAtlas(leaf.atlasPosition+cell),0,1,0);
            else {
                // Integer-local decomposition preserves offsets for large signed origins.
                float3 shifted=(p-floored)+float3(-.5,0,-.5);
                int3 base=cell+int3(floor(shifted));
                float3 t=frac(shifted);
                float interpolated=0;
                uint special=0; // NaN/positive infinity/negative infinity.
                [unroll] for(int z=0;z<2;++z)
                [unroll] for(int y=0;y<2;++y)
                [unroll] for(int x=0;x<2;++x) {
                    float3 w=lerp(1-t,t,float3(x,y,z));
                    float value=sampleAtlas(leaf.atlasPosition+base+int3(x,y,z));
                    uint bits=asuint(value),magnitude=bits&0x7fffffffu;
                    // Explicit IEEE classification survives DXIL reassociation of
                    // zero weights with infinities. Finite interpolation stays native.
                    if(magnitude>=0x7f800000u) {
                        if(magnitude>0x7f800000u || any(w==0)) special|=1;
                        else special|=(bits>>31)!=0?4:2;
                    } else interpolated+=value*w.x*w.y*w.z;
                }
                if((special&1)!=0 || (special&6)==6) interpolated=asfloat(0x7fc00000u);
                else if((special&2)!=0) interpolated=asfloat(0x7f800000u);
                else if((special&4)!=0) interpolated=asfloat(0xff800000u);
                result=float4(interpolated,sampleAtlas(leaf.atlasPosition+base),1,0);
            }
        }
    }
    results[i]=result;
}

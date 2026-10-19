// SPDX-License-Identifier: Apache-2.0
float multiplyRounded(float a, float b) {
    __target_switch {
        case cuda:
            __intrinsic_asm "__fmul_rn($0, $1)";
        default:
            precise float result = a * b;
            return result;
    }
}
float addRounded(float a, float b) {
    __target_switch {
        case cuda:
            __intrinsic_asm "__fadd_rn($0, $1)";
        default:
            precise float result = a + b;
            return result;
    }
}
float3 addRounded3(float3 a,float3 b) { return float3(addRounded(a.x,b.x),addRounded(a.y,b.y),addRounded(a.z,b.z)); }
float3 multiplyRounded3(float3 a,float b) { return float3(multiplyRounded(a.x,b),multiplyRounded(a.y,b),multiplyRounded(a.z,b)); }
struct DepositParameters {
    uint4 resolutionAndApron,brickCountAndWidth,work,options;
    float4 translationAndRadius,offset;
};
ConstantBuffer<DepositParameters> g_Parameters:register(b0);
StructuredBuffer<int4> g_AtlasMap:register(t0);
ByteAddressBuffer g_Positions:register(t1);
ByteAddressBuffer g_Directions:register(t2);
ByteAddressBuffer g_Colors:register(t3);
StructuredBuffer<uint2> g_Cells:register(t4);
StructuredBuffer<uint2> g_Pairs:register(t5);
ByteAddressBuffer g_Previous:register(t6);
ByteAddressBuffer g_PreviousColors:register(t7);
ByteAddressBuffer g_PreviousMaterial:register(t8);
RWByteAddressBuffer g_Destination:register(u0);
RWByteAddressBuffer g_DestinationColors:register(u1);
RWByteAddressBuffer g_DestinationMaterial:register(u2);
[shader("compute")][numthreads(256,1,1)]
void depositOwnedPoints(uint3 id:SV_DispatchThreadID) {
    uint word=id.x+id.y*(65535u*256u),first=word*4u;
    if(first>=g_Parameters.work.x)return;
    uint materialWord=0;
    uint3 resolution=g_Parameters.resolutionAndApron.xyz;
    uint4 grid=g_Parameters.brickCountAndWidth;
    for(uint lane=0;lane<4;++lane) {
        uint index=first+lane;if(index>=g_Parameters.work.x)break;
        uint3 atlas=uint3(index%resolution.x,(index/resolution.x)%resolution.y,index/(resolution.x*resolution.y));
        uint3 brick=atlas/grid.w;
        int4 mapping=g_AtlasMap[(brick.z*grid.y+brick.y)*grid.x+brick.x];
        uint scalar=0,color=0,material=0;
        if(mapping.w>=0) {
            scalar=g_Previous.Load(index*4);
            if(g_Parameters.options.y!=0)color=g_PreviousColors.Load(index*4);
            if(g_Parameters.options.z!=0)material=(g_PreviousMaterial.Load(word*4)>>(lane*8))&255u;
            uint2 range=g_Cells[index];
            uint3 sums=uint3(0);uint colorCount=0;
            float3 voxel=float3(int3(atlas%grid.w)-int3(g_Parameters.resolutionAndApron.w));
            for(uint j=0;j<range.y;++j) {
                uint2 pair=g_Pairs[range.x+j];
                float3 world=addRounded3(asfloat(g_Positions.Load3(pair.x*g_Parameters.work.y)),g_Parameters.translationAndRadius.xyz);
                if(g_Parameters.work.w==2) {
                    float3 shift=multiplyRounded3(asfloat(g_Directions.Load3(pair.x*g_Parameters.work.z)),g_Parameters.offset.x);
                    world=addRounded3(world,shift);
                }
                float3 local=addRounded3(world,-float3(mapping.xyz));
                float3 delta=voxel-local;
                float c=dot(delta,delta)/(g_Parameters.translationAndRadius.w*g_Parameters.translationAndRadius.w);
                scalar=asuint(asfloat(scalar)+(1.0+c*(-3.0+c*(3.0-c))));
                if(g_Parameters.options.z!=0)material=1;
                if(g_Parameters.options.y!=0&&pair.y!=0) {
                    uint value=g_Colors.Load(pair.x*4);
                    if(g_Parameters.options.x==1)color=value;
                    else {sums+=uint3(value&255u,(value>>8)&255u,(value>>16)&255u);++colorCount;}
                }
            }
            if(colorCount!=0) {
                uint3 rgb=sums/colorCount;
                color=rgb.x|(rgb.y<<8)|(rgb.z<<16)|0xff000000u;
            }
        }
        g_Destination.Store(index*4,scalar);
        if(g_Parameters.options.y!=0)g_DestinationColors.Store(index*4,color);
        materialWord|=material<<(lane*8);
    }
    if(g_Parameters.options.z!=0)g_DestinationMaterial.Store(word*4,materialWord);
}

// Packed point decoding, affine conversion and ushort3 quantization.
// SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct PointConversionParameters {uint4 source;float4 minimum,delta,scale,translation;};
ConstantBuffer<PointConversionParameters> g_Parameters:register(b0);
ByteAddressBuffer g_Source:register(t0);
StructuredBuffer<float4> g_Points:register(t1);
RWStructuredBuffer<float4> g_Output:register(u0);
RWByteAddressBuffer g_Quantized:register(u1);
// IEEE binary64 -> binary32 round-to-nearest-even using two 32-bit words.
// This requires neither native doubles nor 64-bit integer operations on SM5.
float decodeDouble(uint2 bits){
    uint sign=bits.y&0x80000000u,exponent=(bits.y>>20)&2047u,high=bits.y&0xfffffu;
    if(exponent==2047u)return asfloat(sign|((high!=0||bits.x!=0)?0x7fc00000u:0x7f800000u));
    if(exponent<873u)return asfloat(sign);
    if(exponent>1150u)return asfloat(sign|0x7f800000u);
    high|=0x100000u;
    if(exponent>=897u){
        uint mantissa=(high<<3)|(bits.x>>29),remainder=bits.x&0x1fffffffu;
        uint increment=(remainder>0x10000000u||(remainder==0x10000000u&&(mantissa&1u)!=0))?1u:0u;
        return asfloat(sign|(((exponent-897u)<<23)+mantissa+increment));
    }
    uint shift=926u-exponent,mantissa=0;bool increment=false;
    if(shift<32u){
        mantissa=(high<<(32u-shift))|(bits.x>>shift);uint remainder=bits.x&((1u<<shift)-1u),half=1u<<(shift-1u);
        increment=remainder>half||(remainder==half&&(mantissa&1u)!=0);
    }else if(shift==32u){
        mantissa=high;increment=bits.x>0x80000000u||(bits.x==0x80000000u&&(mantissa&1u)!=0);
    }else{
        uint upperShift=shift-32u;mantissa=high>>upperShift;uint remainder=high&((1u<<upperShift)-1u),half=1u<<(upperShift-1u);
        increment=remainder>half||(remainder==half&&(bits.x!=0||(mantissa&1u)!=0));
    }
    return asfloat(sign|(mantissa+(increment?1u:0u)));
}
float component(uint address,uint bytes){
    if(bytes==1)return float((g_Source.Load(address&~3u)>>((address&3u)*8u))&255u);
    if(bytes==2)return float((g_Source.Load(address&~3u)>>((address&2u)*8u))&65535u);
    if(bytes==4)return asfloat(g_Source.Load(address));
    return decodeDouble(g_Source.Load2(address));
}
[shader("compute")][numthreads(256,1,1)]
void convertPointPositions(uint3 id:SV_DispatchThreadID){
    uint i=id.x+id.y*(65535u*256u);if(i>=g_Parameters.source.x)return;
    uint bytes=g_Parameters.source.y,address=i*g_Parameters.source.w+g_Parameters.source.z;
    float3 point=float3(component(address,bytes),component(address+bytes,bytes),component(address+2*bytes,bytes));
    point=(g_Parameters.minimum.xyz+point*g_Parameters.delta.xyz)*g_Parameters.scale.xyz+g_Parameters.translation.xyz;
    g_Output[i]=float4(point,1);
}
uint quantizedComponent(uint componentIndex){
    uint pointIndex=componentIndex/3u,axis=componentIndex%3u;
    uint3 bits=asuint(g_Points[pointIndex].xyz);
    // Select integer bits so an unrelated NaN cannot enter finite XYZ components
    // through a compiler-generated floating-point component selection.
    uint selected=axis==0?bits.x:(axis==1?bits.y:bits.z);
    if((selected&0x7fffffffu)>=0x7f800000u)return 0;
    float value=asfloat(selected);
    return uint(saturate((value-g_Parameters.minimum[axis])/g_Parameters.delta[axis])*65535.0);
}
[shader("compute")][numthreads(256,1,1)]
void quantizePointPositions(uint3 id:SV_DispatchThreadID){
    uint i=id.x+id.y*(65535u*256u),components=g_Parameters.source.x*3u;
    if(i>=components/2u+uint(components%2u!=0))return;
    uint packed=quantizedComponent(i*2u);
    if(i*2u+1u<components)packed|=quantizedComponent(i*2u+1u)<<16;
    g_Quantized.Store(i*4u,packed);
}

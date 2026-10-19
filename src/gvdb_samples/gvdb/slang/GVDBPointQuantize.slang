// Authenticated canonical Float3/Float4 -> packed normalized ushort3 -> Float3/Float4.
// SPDX-License-Identifier: Apache-2.0
struct QuantizeParameters { uint4 info; float4 minimum,range; };
ConstantBuffer<QuantizeParameters> g_Parameters:register(b0);
ByteAddressBuffer g_Source:register(t0);
RWByteAddressBuffer g_Codes:register(u0);
RWByteAddressBuffer g_Decoded:register(u1);
uint quantizeComponent(uint component) {
    uint axis=component%3,point=component/3;
    float value=asfloat(g_Source.Load(point*g_Parameters.info.y+axis*4));
    return uint(saturate((value-g_Parameters.minimum[axis])/g_Parameters.range[axis])*65535.0);
}
[shader("compute")][numthreads(256,1,1)]
void encodePointU16(uint3 id:SV_DispatchThreadID) {
    uint word=id.x+id.y*(65535u*256u),components=g_Parameters.info.x*3u;
    if(word>=components/2u+uint(components%2u!=0))return;
    uint value=quantizeComponent(word*2);
    if(word*2+1<components)value|=quantizeComponent(word*2+1)<<16;
    g_Codes.Store(word*4,value);
}
uint code(uint component) {
    return (g_Codes.Load((component/2)*4)>>((component%2)*16))&65535u;
}
[shader("compute")][numthreads(256,1,1)]
void decodePointU16(uint3 id:SV_DispatchThreadID) {
    uint point=id.x+id.y*(65535u*256u);if(point>=g_Parameters.info.x)return;
    uint3 packed=uint3(code(point*3),code(point*3+1),code(point*3+2));
    float3 value=(float3(packed)/65535.0)*g_Parameters.range.xyz+g_Parameters.minimum.xyz;
    uint address=point*g_Parameters.info.y;
    g_Decoded.Store3(address,asuint(value));
    if(g_Parameters.info.z==4)g_Decoded.Store(address+12,g_Source.Load(address+12));
}

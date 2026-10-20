// SPDX-License-Identifier: Apache-2.0
struct Parameters { uint4 work; float4 background; };
ConstantBuffer<Parameters> parameters : register(b0);
StructuredBuffer<float4> color : register(t0);
StructuredBuffer<float> depth : register(t1);
RWStructuredBuffer<uint> output : register(u0);
RWStructuredBuffer<uint> diagnostics : register(u1);
uint encode(float4 value, bool rounded) {
    uint4 bytes=uint4(saturate(value)*255.0+(rounded?.5:0));
    return bytes.x|(bytes.y<<8)|(bytes.z<<16)|(bytes.w<<24);
}
[shader("compute")][numthreads(256,1,1)]
void clearLayers(uint3 id : SV_DispatchThreadID) {
    uint i=id.x+id.y*(65535u*256u);
    if(i<parameters.work.x)output[i]=encode(parameters.background,false);
}
[shader("compute")][numthreads(256,1,1)]
void packLayer(uint3 id : SV_DispatchThreadID) {
    uint i=id.x+id.y*(65535u*256u);
    if(i>=parameters.work.x)return;
    float4 value=color[i];float distance=depth[i];uint error=0;
    if(!all((asuint(value)&uint4(0x7fffffffu))<uint4(0x7f800000u))) {error|=1;value=0;}
    if((asuint(distance)&0x7fffffffu)>0x7f800000u)error|=2;
    if(distance<0)error|=4;
    [branch] if(error!=0)InterlockedOr(diagnostics[0],error);
    output[i]=encode(value,parameters.work.y!=0);
}

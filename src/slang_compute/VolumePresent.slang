// Display the compute result directly from its NVRHI buffer.
// SPDX-License-Identifier: Apache-2.0
cbuffer View : register(b0) { uint4 extent; };
StructuredBuffer<float4> color : register(t0);
[shader("vertex")]
float4 main_vs(uint vertex:SV_VertexID):SV_Position {
    float2 uv=float2((vertex<<1)&2,vertex&2);
    return float4(uv*float2(2,-2)+float2(-1,1),0,1);
}
[shader("fragment")]
float4 main_ps(float4 position:SV_Position):SV_Target {
    uint2 p=min(uint2(position.xy),extent.xy-1);
    return color[p.y*extent.x+p.x];
}

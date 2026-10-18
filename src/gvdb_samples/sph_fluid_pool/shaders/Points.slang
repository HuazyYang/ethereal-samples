// Slang particle rendering reads the simulation's buffers directly.
// SPDX-License-Identifier: Apache-2.0
struct ViewParameters {
    float4 eye;
    float4 right;
    float4 up;
    float4 forward;
    float4 projection; // tan(horizontal/2), tan(vertical/2), radius pixels, near
    float4 extent;     // width, height, velocity tint
};
ConstantBuffer<ViewParameters> g_View:register(b0);
StructuredBuffer<float4> g_Positions:register(t0);
StructuredBuffer<float4> g_Velocities:register(t1);
struct Vertex { float4 position:SV_Position; float4 color:COLOR; float2 corner:TEXCOORD; };
[shader("vertex")]
Vertex main_vs(uint vertex:SV_VertexID,uint particle:SV_InstanceID) {
    float2 corners[6]={float2(-1,-1),float2(1,-1),float2(1,1),float2(-1,-1),float2(1,1),float2(-1,1)};
    float4 p=g_Positions[particle];float3 relative=p.xyz-g_View.eye.xyz;
    float z=dot(relative,g_View.forward.xyz);
    float2 xy=float2(dot(relative,g_View.right.xyz),dot(relative,g_View.up.xyz));
    float2 corner=corners[vertex];
    Vertex result;
    result.position=float4(xy/g_View.projection.xy+corner*g_View.projection.z*2/g_View.extent.xy*z,
        max(z-g_View.projection.w,0),z);
    if(z<g_View.projection.w)result.position=float4(2,2,2,1);
    uint color=asuint(p.w);
    float3 rgb=float3(color&255,(color>>8)&255,(color>>16)&255)/255.f;
    float3 velocity=g_Velocities[particle].xyz;
    result.color=float4(saturate(rgb+dot(velocity,velocity)*g_View.extent.z),1);
    result.corner=corner;return result;
}
[shader("fragment")]
float4 main_ps(Vertex input):SV_Target {
    if(dot(input.corner,input.corner)>1)discard;
    return input.color;
}

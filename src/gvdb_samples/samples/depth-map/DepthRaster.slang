// SPDX-License-Identifier: Apache-2.0
// The original Simple.hlsl mesh illumination and FullScreen.hlsl composition.
struct RasterParameters {
    row_major float4x4 viewProjection;
    float4 eye;
    float4 light;
};
ConstantBuffer<RasterParameters> g_Raster : register(b0);
Texture2D<float4> g_PolygonColor : register(t0);
Texture2D<uint> g_VolumeColor : register(t1);
struct VertexOutput {
    float4 position : SV_Position;
    float4 clipPosition : TEXCOORD0;
    float3 worldPosition : TEXCOORD1;
    float3 worldNormal : TEXCOORD2;
};
[shader("vertex")]
VertexOutput meshVS(float3 position:POSITION,float3 normal:NORMAL) {
    VertexOutput result;
    result.position=mul(float4(position,1),g_Raster.viewProjection);
    result.clipPosition=result.position;
    result.worldPosition=position;result.worldNormal=normal;
    return result;
}
struct PixelOutput {float4 color:SV_Target0;float depth:SV_Target1;};
[shader("fragment")]
PixelOutput meshPS(VertexOutput input) {
    float3 normal=normalize(input.worldNormal);
    if(dot(normal,g_Raster.eye.xyz-input.worldPosition)<0)normal=-normal;
    float diffuse=max(dot(normalize(g_Raster.light.xyz-input.worldPosition),normal),0);
    PixelOutput result;
    result.color=float4(float3(.1+.5*diffuse),1);
    result.depth=input.clipPosition.z/input.clipPosition.w;
    return result;
}
[shader("vertex")]
float4 compositeVS(uint id:SV_VertexID):SV_Position {
    float2 uv=float2(float(id&1),float(id>>1));
    return float4(2*uv.x-1,1-2*uv.y,1,1);
}
[shader("fragment")]
float4 compositePS(float4 position:SV_Position):SV_Target {
    int3 pixel=int3(int2(position.xy),0);
    float4 mesh=g_PolygonColor.Load(pixel);
    uint packed=g_VolumeColor.Load(pixel);
    float4 volume=float4(packed&255,(packed>>8)&255,(packed>>16)&255,(packed>>24)&255)/255.0;
    return float4(lerp(mesh.xyz,volume.xyz,volume.w),1);
}

// SPDX-License-Identifier: Apache-2.0
// Retained gvdbResample coordinates/remap over immutable raw owner allocations.
struct ResampleParameters {
    uint4 resolutionAndApron;
    uint4 brickCountAndWidth;
    uint4 sourceResolutionAndWork;
    float4 rowX;
    float4 rowY;
    float4 rowZ;
    float4 remap;
    uint4 options; // preserve outside, reserved...
    float4 background;
};
ConstantBuffer<ResampleParameters> g_Parameters : register(b0);
ByteAddressBuffer g_Dense : register(t0);
ByteAddressBuffer g_Previous : register(t1);
StructuredBuffer<int4> g_AtlasMap : register(t2);
RWByteAddressBuffer g_Destination : register(u0);
[shader("compute")]
[numthreads(256,1,1)]
void resampleOwnedAtlas(uint3 id : SV_DispatchThreadID) {
    uint index = id.x + id.y * (65535u * 256u);
    if (index >= g_Parameters.sourceResolutionAndWork.w) return;
    uint3 resolution = g_Parameters.resolutionAndApron.xyz;
    uint3 atlas = uint3(index % resolution.x, (index / resolution.x) % resolution.y,
        index / (resolution.x * resolution.y));
    uint4 grid = g_Parameters.brickCountAndWidth;
    uint3 brick = atlas / grid.w;
    int4 mapping = g_AtlasMap[(brick.z * grid.y + brick.y) * grid.x + brick.x];
    uint bits = 0;
    if (mapping.w >= 0) {
        bits = g_Parameters.options.x != 0 ? g_Previous.Load(index * 4u) : asuint(g_Parameters.background.x);
        float4 world = float4(float3(mapping.xyz + int3(atlas % grid.w) - int(g_Parameters.resolutionAndApron.w)),1);
        float3 source = float3(dot(g_Parameters.rowX,world),dot(g_Parameters.rowY,world),dot(g_Parameters.rowZ,world));
        // Finite overflow/cancellation also cannot reach an undefined integer cast.
        if (all(isfinite(source)) && all(source >= -2147483648.0) && all(source < 2147483648.0)) {
            int3 position = int3(source);
            uint3 extent = g_Parameters.sourceResolutionAndWork.xyz;
            if (all(position >= 0) && all(uint3(position) < extent)) {
                uint offset = (uint(position.z) * extent.y + uint(position.y)) * extent.x + uint(position.x);
                float value = asfloat(g_Dense.Load(offset * 4u));
                float4 range = g_Parameters.remap;
                value = range.z + (value - range.x) * (range.w - range.z) / (range.y - range.x);
                bits = asuint(value);
            }
        }
    }
    g_Destination.Store(index * 4u,bits);
}

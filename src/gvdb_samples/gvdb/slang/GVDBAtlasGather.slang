// SPDX-License-Identifier: Apache-2.0
// Ordinary and inverse-distance density/level-set gathers over immutable raw owner allocations.
struct GatherParameters {
    uint4 resolutionAndApron;
    uint4 brickCountAndWidth;
    uint4 work; // atlas voxels, subcell width, cells per leaf side, position stride
    uint4 options; // level set, accumulate, color enabled, cells per leaf
    float4 translationAndRadius;
};
ConstantBuffer<GatherParameters> g_Parameters : register(b0);
StructuredBuffer<int4> g_AtlasMap : register(t0);
ByteAddressBuffer g_Positions : register(t1);
ByteAddressBuffer g_Colors : register(t2);
StructuredBuffer<uint2> g_Cells : register(t3);
ByteAddressBuffer g_Indices : register(t4);
ByteAddressBuffer g_Previous : register(t5);
ByteAddressBuffer g_PreviousColors : register(t6);
RWByteAddressBuffer g_Destination : register(u0);
RWByteAddressBuffer g_DestinationColors : register(u1);
float4 unpackColor(uint packed) {
    return float4(packed & 255u,(packed >> 8) & 255u,(packed >> 16) & 255u,packed >> 24) / 255.0;
}
uint packColor(float4 color) {
    // Ordinary gather is finite and nonnegative, with each component at most 2.
    // Preserve legacy lane wrapping, including fresh initial alpha's extra unit.
    uint4 bytes = uint4(color * 255.0) & uint4(255u);
    return bytes.x | (bytes.y << 8) | (bytes.z << 16) | (bytes.w << 24);
}
void gatherAtlas(uint3 id, bool inverseDistance) {
    uint index = id.x + id.y * (65535u * 256u);
    if (index >= g_Parameters.work.x) return;
    uint3 resolution = g_Parameters.resolutionAndApron.xyz;
    uint3 atlas = uint3(index % resolution.x,(index / resolution.x) % resolution.y,
        index / (resolution.x * resolution.y));
    uint4 grid = g_Parameters.brickCountAndWidth;
    uint3 brick = atlas / grid.w;
    int4 mapping = g_AtlasMap[(brick.z * grid.y + brick.y) * grid.x + brick.x];
    bool colors = g_Parameters.options.z != 0;
    uint bits = 0, colorBits = 0;
    if (mapping.w >= 0) {
        bits = g_Previous.Load(index * 4u);
        if (colors) colorBits = g_PreviousColors.Load(index * 4u);
        uint3 local = atlas % grid.w;
        uint apron = g_Parameters.resolutionAndApron.w;
        if (all(local >= apron) && all(local < grid.w - apron)) {
            uint3 interior = local - apron;
            uint3 cell = interior / g_Parameters.work.y;
            uint side = g_Parameters.work.z;
            uint cellIndex = uint(mapping.w) * g_Parameters.options.w + (cell.z * side + cell.y) * side + cell.x;
            uint2 points = g_Cells[cellIndex];
            bool accumulate = g_Parameters.options.y != 0;
            if (!accumulate) {
                bits = asuint(g_Parameters.options.x != 0 ? 3.0 : 0.0);
                colorBits = 0;
            }
            if (points.y != 0) {
                float3 world = float3(mapping.xyz + int3(interior));
                float4 color = inverseDistance ? float4(0) : (accumulate ? unpackColor(colorBits) : float4(0,0,0,1));
                for (uint j = 0; j < points.y; ++j) {
                    uint point = g_Indices.Load((points.x + j) * 4u);
                    float3 position = asfloat(g_Positions.Load3(point * g_Parameters.work.w));
                    position += g_Parameters.translationAndRadius.xyz;
                    float3 delta = position - world;
                    float distance = sqrt(dot(delta,delta));
                    float candidate = g_Parameters.options.x != 0 ? distance - g_Parameters.translationAndRadius.w
                        : g_Parameters.translationAndRadius.w - distance;
                    // Explicit ordered comparison preserves the seed's exact bits on
                    // ties and NaNs, independent of backend min/max NaN conventions.
                    if (g_Parameters.options.x != 0 ? candidate < asfloat(bits) : candidate > asfloat(bits))
                        bits = asuint(candidate);
                    if (colors) {
                        float4 contribution = unpackColor(g_Colors.Load(point * 4u));
                        if (inverseDistance) {
                            // No weight normalization or epsilon. Define the zero-distance
                            // limit lane by lane and saturate before any integer cast.
                            if (distance == 0) contribution = float4(contribution > float4(0));
                            else contribution /= distance;
                            color = saturate(color + contribution);
                        } else color += contribution;
                    }
                }
                if (colors) colorBits = packColor(inverseDistance ? color : color / float(points.y + (accumulate ? 1u : 0u)));
            }
        }
    }
    g_Destination.Store(index * 4u,bits);
    if (colors) g_DestinationColors.Store(index * 4u,colorBits);
}
[shader("compute")][numthreads(256,1,1)]
void gatherOwnedAtlas(uint3 id : SV_DispatchThreadID) { gatherAtlas(id, false); }
[shader("compute")][numthreads(256,1,1)]
void gatherOwnedAtlasInverseDistance(uint3 id : SV_DispatchThreadID) { gatherAtlas(id, true); }

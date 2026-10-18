// SPDX-License-Identifier: Apache-2.0
// Legacy packed-XY column order, with inactive slots explicitly excluded.
struct ReductionParameters {
    uint4 resolutionAndInterior;
    uint4 brickCountAndWidth;
    uint4 work; // columns, packed X resolution, apron, reserved
};
ConstantBuffer<ReductionParameters> g_Parameters : register(b0);
ByteAddressBuffer g_Atlas : register(t0);
StructuredBuffer<int4> g_AtlasMap : register(t1);
RWStructuredBuffer<float> g_Columns : register(u0);
[shader("compute")]
[numthreads(256,1,1)]
void reduceActiveColumns(uint3 id : SV_DispatchThreadID) {
    uint column = id.x + id.y * (65535u * 256u);
    if (column >= g_Parameters.work.x) return;
    uint interior = g_Parameters.resolutionAndInterior.w;
    uint2 packed = uint2(column % g_Parameters.work.y, column / g_Parameters.work.y);
    uint2 brick = packed / interior;
    uint2 local = packed % interior;
    uint4 grid = g_Parameters.brickCountAndWidth;
    uint2 atlasXY = brick * grid.w + local + g_Parameters.work.z;
    float sum = 0;
    for (uint bz = 0; bz < grid.z; ++bz) {
        uint slot = (bz * grid.y + brick.y) * grid.x + brick.x;
        if (g_AtlasMap[slot].w < 0) continue;
        uint firstZ = bz * grid.w + g_Parameters.work.z;
        for (uint z = 0; z < interior; ++z) {
            uint index = ((firstZ + z) * g_Parameters.resolutionAndInterior.y + atlasXY.y)
                * g_Parameters.resolutionAndInterior.x + atlasXY.x;
            sum += asfloat(g_Atlas.Load(index * 4u));
        }
    }
    g_Columns[column] = sum;
}

/*
 * create_hi_z_cs.hlsl - builds the 5-level "far Z" (max depth) pyramid used by the meshlet
 * Hi-Z occlusion test (asteroidTS.hlsl, _MESHLETS_HI_Z=1).
 *
 * Each thread reduces an 8x8 pixel tile of the depth buffer with 16 Gathers into one texel of
 * u_ZFar[0]; a 16x16 group then reduces its 16x16 level-0 texels through groupshared memory
 * into 8x8, 4x4, 2x2 and 1x1 texels of u_ZFar[1..4].
 *
 * Dispatch: ceil(depthWidth / (8 * 16)) x ceil(depthHeight / (8 * 16)) groups.
 *
 * Compute shader, cs_6_0, not ported (no NVAPI usage).
 */

#define TILE_SIZE   8       // depth pixels per level-0 texel (per axis)
#define GROUP_SIZE  16

Texture2D<float>   t_ZBuffer : register(t0);
RWTexture2D<float> u_ZFar[5] : register(u0);
SamplerState       s_Sampler : register(s0);

groupshared float s_ReductionData[GROUP_SIZE][GROUP_SIZE];

// Maximum depth over the tileSize x tileSize pixel tile 'tile'.
float GetFarZFromTile(uint2 tile, int tileSize)
{
    uint2 dims;
    t_ZBuffer.GetDimensions(dims.x, dims.y);
    float2 invDims = 1.0 / float2(dims);

    // Gather at the shared corner of pixels (0,0)..(1,1) of the tile, then step by 2 pixels.
    float2 uv = (float2(tile) * tileSize + 1.0) * invDims;

    float value = 0;

    [unroll]
    for (int y = 0; y < tileSize; y += 2)
    {
        [unroll]
        for (int x = 0; x < tileSize; x += 2)
        {
            float4 depths = t_ZBuffer.Gather(s_Sampler, uv, int2(x, y));
            value = max(value, max(max(depths.x, depths.y), max(depths.z, depths.w)));
        }
    }

    return value;
}

[numthreads(GROUP_SIZE, GROUP_SIZE, 1)]
void main(uint3 groupId : SV_GroupID, uint3 globalId : SV_DispatchThreadID, uint3 threadId : SV_GroupThreadID)
{
    float value = GetFarZFromTile(globalId.xy, TILE_SIZE);
    u_ZFar[0][globalId.xy] = value;

    uint size = GROUP_SIZE;

    [unroll]
    for (uint level = 1; level < 5; level++)
    {
        if (all(threadId.xy < size))
            s_ReductionData[threadId.y][threadId.x] = value;

        GroupMemoryBarrierWithGroupSync();

        size >>= 1;

        if (all(threadId.xy < size))
        {
            uint2 src = threadId.xy * 2;
            float a = s_ReductionData[src.y + 0][src.x + 0];
            float b = s_ReductionData[src.y + 0][src.x + 1];
            float c = s_ReductionData[src.y + 1][src.x + 0];
            float d = s_ReductionData[src.y + 1][src.x + 1];

            value = max(max(a, b), max(c, d));
            u_ZFar[level][groupId.xy * size + threadId.xy] = value;
        }

        GroupMemoryBarrierWithGroupSync();
    }
}

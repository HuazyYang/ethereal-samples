#ifndef WW_REGISTER
#ifdef WAVEWORKS_SPACE
#define WW_REGISTER(reg) register(reg, WAVEWORKS_SPACE)
#else
#define WW_REGISTER(reg) register(reg)
#endif
#endif
#ifndef BEACH_SHALLOW_WATER_HLSLI
#define BEACH_SHALLOW_WATER_HLSLI
cbuffer BeachShallowConstants : WW_REGISTER(b3)
{
    float2 g_sweOrigin;
    float2 g_sweInverseExtent;
    float2 g_sweCellSize;
    float g_sweEnabled;
    float g_sweSeaLevel;
};
#ifdef BEACH_SHALLOW_DOMAIN
Texture2D<float4> g_sweSurface : WW_REGISTER(t3);
#else
Texture2D<float4> g_sweSurface : WW_REGISTER(t14);
Texture2D<float4> g_sweFlow : WW_REGISTER(t15);
#endif

float2 BeachShallowUv(float2 world) { return (world - g_sweOrigin) * g_sweInverseExtent; }
float BeachShallowWeight(float2 world)
{
    float2 uv = BeachShallowUv(world);
    // Overlap of two simulation domains, not a water-color ramp. Keep the
    // transparent border outside the cove and blend displacement/derivatives.
    float2 border = min(uv, 1.0 - uv) / g_sweInverseExtent;
    return g_sweEnabled * smoothstep(0.0, 12.0, min(border.x, border.y));
}
float BeachShallowLowPassLod(float uvScale, float textureSize)
{
    // Remove the FFT wavelengths resolved by the finite-volume mesh. Both
    // displacement and gradient use the same low-pass footprint, leaving
    // capillary/short-wave detail without doubling the incoming gravity wave.
    return max(0.0, log2(max(2.0 * max(g_sweCellSize.x, g_sweCellSize.y) *
        uvScale * textureSize, 1.0)));
}
#endif

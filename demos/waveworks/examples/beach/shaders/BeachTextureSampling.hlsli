#ifndef BEACH_TEXTURE_SAMPLING_HLSLI
#define BEACH_TEXTURE_SAMPLING_HLSLI

// Triangular stochastic tiling, following Heitz/Neyret and Deliot/Heitz.
// This blends MATERIAL INPUT samples, never lit colors. Ordinary samples use
// convex blending; the coastal albedo additionally preserves source luminance
// with the Gaussian/inverse-histogram encoding below.
// Shared patch transforms keep diffuse/normal maps registered. Explicit
// derivatives avoid false coarse mips at randomly translated tile boundaries.
uint BeachTextureHash(uint v)
{
    v ^= v >> 16; v *= 0x7feb352du;
    v ^= v >> 15; v *= 0x846ca68bu;
    return v ^ (v >> 16);
}
uint BeachPatchHash(int2 cell)
{
    return BeachTextureHash(asuint(cell.x) ^ BeachTextureHash(asuint(cell.y) + 0x9e3779b9u));
}
float2 BeachPatchRotate(float2 uv, uint quadrant)
{
    if (quadrant == 1) return float2(-uv.y, uv.x);
    if (quadrant == 2) return -uv;
    if (quadrant == 3) return float2(uv.y, -uv.x);
    return uv;
}
struct BeachTexturePatches
{
    float2 uv0, uv1, uv2;
    float2 dx, dy;
    uint3 rotation;
    float3 weight;
};
float2 BeachPatchUv(float2 uv, int2 cell, out uint rotation)
{
    uint h = BeachPatchHash(cell);
    rotation = h & 3u;
    float2 offset = float2(h & 65535u, BeachTextureHash(h) & 65535u) / 65536.0;
    return BeachPatchRotate(uv, rotation) + offset;
}
BeachTexturePatches BeachMakeTexturePatches(float2 uv, bool stochastic)
{
    BeachTexturePatches p;
    p.dx = ddx(uv); p.dy = ddy(uv);
    if (!stochastic)
    {
        p.uv0 = p.uv1 = p.uv2 = uv;
        p.rotation = 0;
        p.weight = float3(1, 0, 0);
        return p;
    }
    // Coordinates in an equilateral triangular lattice.
    float2 skew = float2(uv.x - uv.y * 0.577350269, uv.y * 1.154700538);
    int2 cell = int2(floor(skew));
    float2 f = frac(skew);
    int2 c0, c1, c2;
    if (f.x + f.y <= 1)
    {
        c0 = cell; c1 = cell + int2(1, 0); c2 = cell + int2(0, 1);
        p.weight = float3(1 - f.x - f.y, f.x, f.y);
    }
    else
    {
        c0 = cell + 1; c1 = cell + int2(0, 1); c2 = cell + int2(1, 0);
        p.weight = float3(f.x + f.y - 1, 1 - f.x, 1 - f.y);
    }
    // Narrower transitions retain scan detail and ease in the entering patch
    // across triangle edges. The weights are nonnegative and sum to one.
    p.weight *= p.weight;
    p.weight /= dot(p.weight, 1.0.xxx);
    p.uv0 = BeachPatchUv(uv, c0, p.rotation.x);
    p.uv1 = BeachPatchUv(uv, c1, p.rotation.y);
    p.uv2 = BeachPatchUv(uv, c2, p.rotation.z);
    return p;
}
float3 BeachSampleMaterialColor(Texture2D tex, SamplerState samp, BeachTexturePatches p)
{
    float3 a = tex.SampleGrad(samp, p.uv0, BeachPatchRotate(p.dx, p.rotation.x),
        BeachPatchRotate(p.dy, p.rotation.x)).bgr;
    float3 b = tex.SampleGrad(samp, p.uv1, BeachPatchRotate(p.dx, p.rotation.y),
        BeachPatchRotate(p.dy, p.rotation.y)).bgr;
    float3 c = tex.SampleGrad(samp, p.uv2, BeachPatchRotate(p.dx, p.rotation.z),
        BeachPatchRotate(p.dy, p.rotation.z)).bgr;
    return a * p.weight.x + b * p.weight.y + c * p.weight.z;
}
float3 BeachSampleContrastColor(Texture2D tex, Texture2D<float> inverseLuminance,
    SamplerState samp, SamplerState lookupSampler, BeachTexturePatches p, float2 materialUv)
{
    float4 a = tex.SampleGrad(samp, p.uv0, BeachPatchRotate(p.dx, p.rotation.x),
        BeachPatchRotate(p.dy, p.rotation.x)).bgra;
    float4 b = tex.SampleGrad(samp, p.uv1, BeachPatchRotate(p.dx, p.rotation.y),
        BeachPatchRotate(p.dy, p.rotation.y)).bgra;
    float4 c = tex.SampleGrad(samp, p.uv2, BeachPatchRotate(p.dx, p.rotation.z),
        BeachPatchRotate(p.dy, p.rotation.z)).bgra;
    float4 blend = a * p.weight.x + b * p.weight.y + c * p.weight.z;
    // Heitz/Neyret variance-preserving Gaussian blend, applied to photographed
    // luminance only. The LUT is derived from the source, not an authored grade.
    float gaussian = .5 + (blend.a - .5) * rsqrt(dot(p.weight, p.weight));
    uint width, levels;
    inverseLuminance.GetDimensions(width, levels);
    float mip = clamp(tex.CalculateLevelOfDetail(samp, materialUv), 0, float(levels - 1));
    float2 lookup = float2(clamp(gaussian, .5 / width, 1 - .5 / width), (mip + .5) / levels);
    float luminance = inverseLuminance.SampleLevel(lookupSampler, lookup, 0);
    return blend.rgb * luminance / max(dot(blend.rgb, float3(.2126, .7152, .0722)), 1e-6);
}
float3 BeachPatchNormal(Texture2D tex, SamplerState samp, float2 uv,
    float2 dx, float2 dy, uint rotation)
{
    float3 n = tex.SampleGrad(samp, uv, BeachPatchRotate(dx, rotation),
        BeachPatchRotate(dy, rotation)).bgr * 2 - 1;
    n.y = -n.y; // Source DirectX normal convention.
    // Pull texture-space slopes back into the unrotated material frame.
    n.xy = BeachPatchRotate(n.xy, (4u - rotation) & 3u);
    return n;
}
float3 BeachSampleMaterialNormal(Texture2D tex, SamplerState samp,
    BeachTexturePatches p, float strength, out float variance)
{
    float3 n = BeachPatchNormal(tex, samp, p.uv0, p.dx, p.dy, p.rotation.x) * p.weight.x +
        BeachPatchNormal(tex, samp, p.uv1, p.dx, p.dy, p.rotation.y) * p.weight.y +
        BeachPatchNormal(tex, samp, p.uv2, p.dx, p.dy, p.rotation.z) * p.weight.z;
    // Preserve filtered normal length until Toksvig variance is recovered.
    float meanLength = saturate(length(n));
    variance = max(1 - meanLength, 0) / max(meanLength, 0.05) * strength * strength;
    return normalize(float3(n.xy * strength, max(n.z, 0.05)));
}
#endif

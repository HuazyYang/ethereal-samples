/*
* Asteroids (2018) - procedural asteroid surface material and LOD transition dithering.
*
* Reconstructed from the _ASTEROIDS=1 permutations of forward_ps, gbuffer_ps and material_id_ps
* (inlined EvaluateAsteroidsMaterial, fbmd, safeNormalize, colorRamp).
*
* The asteroid pixel shaders are fed by asteroidMS: ATTR1 = (worldPos.xyz, uv.x),
* ATTR2 = (normal.xyz, uv.y), ALPHA_LOD = (transition alpha, fade-in flag, distance fade alpha, LOD).
* worldPos is camera relative (translated by FrameCB::preViewTranslation).
*
* Requires before inclusion: scene_material_2018.hlsli (g_Material, t_Diffuse, s_MaterialSampler)
* and, unless ASTEROID_MATERIAL_DITHER_ONLY is defined, the cbFrame constant buffer (FrameCB).
* t_RandomsTexture is only declared when ASTEROID_MATERIAL_USE_DITHER is defined.
*/

#ifndef ASTEROID_MATERIAL_HLSLI
#define ASTEROID_MATERIAL_HLSLI

#if ASTEROID_MATERIAL_USE_DITHER

Texture3D t_RandomsTexture : register(t15, space1);

// Dithered cross-fade between two asteroid LODs. The mesh shader emits the transition alpha in
// alphaLod.x and whether this LOD is fading in (alphaLod.y != 0) or out. Returns the random value
// so that callers can reuse it for the view distance fade.
float LodTransitionDither(float4 position, float4 alphaLod)
{
    int3 randomCoord = int3(position.xy, frac(alphaLod.x) * 64.0) & 63;
    float random = t_RandomsTexture.Load(int4(randomCoord, 0)).x;

    if (alphaLod.y != 0)
    {
        if (random >= alphaLod.x)
            discard;
    }
    else
    {
        if (random < alphaLod.x)
            discard;
    }

    return random;
}

#endif // ASTEROID_MATERIAL_USE_DITHER

#if !ASTEROID_MATERIAL_DITHER_ONLY

// Value noise with analytic derivatives and fBm, after Inigo Quilez
// ("Value Noise Derivatives" / "fbm derivatives"). The noise core runs at minimum precision
// (min16float in the 2018 shaders; native half when compiled with -enable-16bit-types, which
// is what min16float maps to in that mode anyway).
#if __HLSL_ENABLE_16_BIT
typedef float16_t noise_half;
typedef float16_t3 noise_half3;
#else
typedef min16float noise_half;
typedef min16float3 noise_half3;
#endif

float hash(float3 p)
{
    p = 50.0 * frac(p * 0.3183099 + float3(0.71, 0.113, 0.419));
    return -1.0 + 2.0 * frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// returns (value, d/dx, d/dy, d/dz)
float4 noised(float3 x)
{
    float3 p = floor(x);
    noise_half3 w = (noise_half3)frac(x);

    noise_half3 u = w * w * (3.0 - 2.0 * w);
    noise_half3 du = 6.0 * w * (1.0 - w);

    noise_half a = (noise_half)hash(p + float3(0, 0, 0));
    noise_half b = (noise_half)hash(p + float3(1, 0, 0));
    noise_half c = (noise_half)hash(p + float3(0, 1, 0));
    noise_half d = (noise_half)hash(p + float3(1, 1, 0));
    noise_half e = (noise_half)hash(p + float3(0, 0, 1));
    noise_half f = (noise_half)hash(p + float3(1, 0, 1));
    noise_half g = (noise_half)hash(p + float3(0, 1, 1));
    noise_half h = (noise_half)hash(p + float3(1, 1, 1));

    noise_half k0 = a;
    noise_half k1 = b - a;
    noise_half k2 = c - a;
    noise_half k3 = e - a;
    noise_half k4 = a - b - c + d;
    noise_half k5 = a - c - e + g;
    noise_half k6 = a - b - e + f;
    noise_half k7 = -a + b + c - d + e - f - g + h;

    return float4(k0 + k1 * u.x + k2 * u.y + k3 * u.z + k4 * u.x * u.y + k5 * u.y * u.z + k6 * u.z * u.x + k7 * u.x * u.y * u.z,
        du * noise_half3(k1 + k4 * u.y + k6 * u.z + k7 * u.y * u.z,
                         k2 + k5 * u.z + k4 * u.x + k7 * u.z * u.x,
                         k3 + k6 * u.x + k5 * u.y + k7 * u.x * u.y));
}

// Octave rotation matrices of the fBm: m3 applied to the coordinate, m3i = transpose(m3) to the derivative matrix.
// gbuffer_ps.hlsl sets "#pragma pack_matrix(row_major)", under which DXC lowers mul() on these static const
// matrices with the operands effectively transposed; the swapped-operand form below compiles to the rotation
// of the shipped DXIL.
static const float3x3 m3 = float3x3( 0.00,  0.80,  0.60,
                                    -0.80,  0.36, -0.48,
                                    -0.60, -0.48,  0.64);
static const float3x3 m3i = float3x3(0.00, -0.80, -0.60,
                                     0.80,  0.36, -0.48,
                                     0.60, -0.48,  0.64);

// returns (value, gradient)
float4 fbmd(float3 x, int octaves)
{
    float f = 1.98;     // could be 2.0
    float s = 0.49;     // could be 0.5
    float a = 0.0;
    float b = 0.5;
    float3 d = 0.0;
    float3x3 m = float3x3(1.0, 0.0, 0.0,
                          0.0, 1.0, 0.0,
                          0.0, 0.0, 1.0);

    for (int i = 0; i < octaves; i++)
    {
        float4 n = noised(x);
        a += b * n.x;               // accumulate values
        d += b * mul(n.yzw, m);     // accumulate derivatives: d += b * (m * grad)
        b *= s;
        x = f * mul(x, m3);
        m = f * mul(m, m3i);
    }

    return float4(a, d);
}

float3 safeNormalize(float3 v)
{
    float lengthSquared = dot(v, v);
    if (lengthSquared == 0)
        return 0;

    return v / sqrt(lengthSquared);
}

float3 colorRamp(float t)
{
    // One constant array per channel: a static const float3 array was compiled by DXC 1.8 into a writable global that is
    // refilled (12 stores) on every pixel; the shipped DXIL keeps it as constant data.
    static const float paletteR[4] = { 240.0 / 255.0, 213.0 / 255.0, 127.0 / 255.0,  25.0 / 255.0 };
    static const float paletteG[4] = { 247.0 / 255.0, 216.0 / 255.0, 138.0 / 255.0,  35.0 / 255.0 };
    static const float paletteB[4] = { 253.0 / 255.0, 221.0 / 255.0, 153.0 / 255.0,  50.0 / 255.0 };

    int i = int(floor(t));
    float3 c0 = float3(paletteR[i], paletteG[i], paletteB[i]);
    float3 c1 = float3(paletteR[i + 1], paletteG[i + 1], paletteB[i + 1]);
    return lerp(c0, c1, frac(t));
}

SurfaceParams EvaluateAsteroidsMaterial(float3 worldPos, float2 uv, float3 normal)
{
    SurfaceParams surface = (SurfaceParams)0;

    surface.worldPos = worldPos;

    float normalLength = length(normal);
    if (normalLength == 0)
        surface.geometryNormal = normalize(cross(ddy(worldPos), ddx(worldPos)));
    else
        surface.geometryNormal = normal / normalLength;

    // Noise space: untranslated world position in units of 100.
    float3 noisePos = (worldPos - cbFrame.preViewTranslation.xyz) * 0.01;

    // Pick the number of octaves from the screen-space footprint of the noise.
    float3 noiseDdx = ddx(noisePos);
    float3 noiseDdy = ddy(noisePos);
    float footprint = sqrt(dot(noiseDdx, noiseDdx) + dot(noiseDdy, noiseDdy));
    int octaves = clamp(int(10.0 - log2(footprint * 500.0)), 0, 10);

    float4 fbm = fbmd(noisePos, octaves);

    float3 noiseNormal = safeNormalize(fbm.yzw);
    surface.normal = normalize(lerp(surface.geometryNormal, noiseNormal, 0.15));

    float3 albedo = colorRamp((fbm.x + 1.0) * 1.5);

    if (g_Material.useDiffuseTexture)
        albedo = lerp(albedo, t_Diffuse.Sample(s_MaterialSampler, uv).rgb, 0.85);

    surface.diffuseColor = albedo * (1.0 - g_Material.specularColor) * g_Material.diffuseColor;
    surface.specularColor = g_Material.specularColor;
    surface.emissiveColor = g_Material.emissiveColor;
    surface.opacity = 1.0;
    surface.roughness = g_Material.roughness;

    return surface;
}

#endif // !ASTEROID_MATERIAL_DITHER_ONLY

#endif // ASTEROID_MATERIAL_HLSLI

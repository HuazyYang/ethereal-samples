/*
 * High-quality blit: Lanczos-3 resampling of 'tex' with 13 bilinear taps.
 *
 * The 6x6 Lanczos-3 footprint is reduced to 5 taps per axis by merging the two center texels into
 * one bilinear sample (Sample[2]), and the 5x5 tap grid is reduced to the 13 taps of a diamond
 * (|x-2| + |y-2| <= 2). The sin() terms of all six weights are derived from a single sin/cos pair
 * with the angle-addition identities (sin(pi*(x-k)) = +-sin(pi*x), sin(pi*(x-k)/3) expanded
 * around pi/3 steps); the weights are only relative, the result is normalized by the weight sum.
 *
 * Note: the outermost weight (Weight[0], distance f+2) has half the magnitude of the exact Lanczos
 * value relative to the other five (it lacks the common factor 2). This is how the shipped shader
 * computes it and is kept as is.
 *
 * Entry point: main (ps_6_0).   Reconstructed from DXIL (hq_blit_ps).
 */

Texture2D tex : register(t0);
SamplerState samp : register(s0);

static const float PI = 3.14159265;
static const float SQRT3 = 1.7320508;

float4 main(float4 i_position : SV_Position, float2 i_uv : UV) : SV_Target
{
    uint width, height;
    tex.GetDimensions(width, height);
    float2 textureSize = float2(width, height);
    float2 invTextureSize = 1.0 / textureSize;

    float2 samplePos = i_uv * textureSize;
    float2 texelFloor = floor(samplePos - 0.5);
    float2 texelCenter = texelFloor + 0.5;
    float2 f = samplePos - texelCenter;            // [0, 1)

    // Distances to the six texel centers: f+2, f+1, f, f-1, f-2, f-3
    float2 x0 = f + 2.0;
    float2 s = sin(PI * x0);
    float2 s3 = sin(PI / 3.0 * x0);
    float2 c3 = cos(PI / 3.0 * x0);

    float2 w0 = s * s3 / (x0 * x0);
    float2 w1 = -(s * (s3 - SQRT3 * c3)) / ((f + 1.0) * (f + 1.0));
    float2 w2 = (-SQRT3 * c3 - s3) * s / (f * f);
    float2 w3 = 2.0 * s * s3 / ((f - 1.0) * (f - 1.0));
    float2 w4 = (SQRT3 * c3 - s3) * s / ((f - 2.0) * (f - 2.0));
    float2 w5 = -(s * (SQRT3 * c3 + s3)) / ((f - 3.0) * (f - 3.0));

    float2 Weight[5];
    float2 Sample[5];

    Weight[0] = w0;
    Weight[1] = w1;
    Weight[2] = w3 + w2;
    Weight[3] = w4;
    Weight[4] = w5;

    Sample[0] = invTextureSize * (texelCenter - 2.0);
    Sample[1] = invTextureSize * (texelCenter - 1.0);
    Sample[2] = (texelCenter + w3 / Weight[2]) * invTextureSize;
    Sample[3] = invTextureSize * (texelCenter + 2.0);
    Sample[4] = invTextureSize * (texelCenter + 3.0);

    float4 result = 0;

    [unroll]
    for (int x = 0; x < 5; x++)
    {
        [unroll]
        for (int y = 0; y < 5; y++)
        {
            if (abs(x - 2) + abs(y - 2) > 2)
                continue;

            float3 color = tex.SampleLevel(samp, float2(Sample[x].x, Sample[y].y), 0).rgb;
            result += float4(color, 1.0) * Weight[x].x * Weight[y].y;
        }
    }

    return float4(result.rgb / result.w, 1.0);
}

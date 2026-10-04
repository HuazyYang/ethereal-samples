/*
* Asteroids (2018) - high quality screenshot accumulation pixel shader.
*
* 'tex' holds 64 jittered renderings of the frame (8x8 sub-pixel grid, one per array slice).
* Every output pixel resolves the 5x5 neighbourhood of all 64 samples with a separable
* Lanczos-2 filter. Filtering happens in ST.2084 (PQ) space to avoid ringing on HDR values,
* and the result is converted back to linear (nits).
*
* No permutations.
*/

Texture2DArray tex : register(t0);

static const float PI = 3.14159265;

// SMPTE ST.2084 constants
static const float PQ_m1 = 2610.0 / 4096.0 / 4.0;
static const float PQ_m2 = 2523.0 / 4096.0 * 128.0;
static const float PQ_c1 = 3424.0 / 4096.0;
static const float PQ_c2 = 2413.0 / 4096.0 * 32.0;
static const float PQ_c3 = 2392.0 / 4096.0 * 32.0;

float3 LinearToPQ(float3 color)
{
    float3 Lp = pow(max(color / 10000.0, 0), PQ_m1);
    return pow((PQ_c1 + PQ_c2 * Lp) / (1.0 + PQ_c3 * Lp), PQ_m2);
}

float3 PQToLinear(float3 color)
{
    float3 Np = pow(max(color, 0), 1.0 / PQ_m2);
    float3 L = pow(max(Np - PQ_c1, 0) / (PQ_c2 - PQ_c3 * Np), 1.0 / PQ_m1);
    return L * 10000.0;
}

// Lanczos-2 kernel without its constant normalization (the weights are normalized at the end).
float2 Lanczos2(float2 x)
{
    x = max(abs(x), 1e-5);
    float2 weight = sin(PI * x) * sin(PI * x * 0.5) / (x * x);
    return weight * step(x, 2.0);
}

void main(
    in float4 i_position : SV_Position,
    in float2 i_uv : UV,
    out float4 o_color : SV_Target0)
{
    float4 TotalColor = 0;

    for (int X = -2; X <= 2; X++)
    {
        for (int Y = -2; Y <= 2; Y++)
        {
            for (int nSample = 0; nSample < 64; nSample++)
            {
                float3 Pixel = tex.Load(int4(int2(i_position.xy + float2(X, Y)), nSample, 0)).rgb;
                float2 SubpixelOffset = (float2(nSample & 7, nSample >> 3) + 0.5) / 8.0;

                if (any(!isfinite(Pixel)))
                    Pixel = 0;

                Pixel = LinearToPQ(Pixel);

                float2 Weight2 = Lanczos2(float2(X, Y) + 0.5 - SubpixelOffset);
                float Weight = Weight2.x * Weight2.y;

                TotalColor += float4(Pixel * Weight, Weight);
            }
        }
    }

    o_color = float4(PQToLinear(TotalColor.rgb / TotalColor.a), 1.0);
}

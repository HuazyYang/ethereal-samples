// The cbuffer fields are deliberately declared without packoffset. Natural
// packing gives the identical layout (eight 4-byte scalars at offsets 0..28),
// but FXC 10.1 emits `iadd rN, l(-1), cb0[0].x` for `size - 1` when the field
// carries a packoffset annotation and the shipped `iadd rN, cb0[0].x, l(-1)`
// when it does not. That one instruction is in EdgeAttenuation below.
cbuffer CS_CBUFFER : register(b0)
{
    uint g_simulationGridSize;
    uint g_32minusLog2Resolution;
    float g_simulationWorldspaceSize;
    float g_dT;
    float g_dampening;
    uint g_numDisturbanceSamples;
    float g_verticalScaler;
    float g_lateralScaler;
};

StructuredBuffer<float4> g_input1 : register(t0);
StructuredBuffer<float4> g_input2 : register(t1);
RWStructuredBuffer<float4> g_output1 : register(u0);
RWStructuredBuffer<float4> g_output2 : register(u1);
RWTexture2D<float4> g_displacementsOutput : register(u2);

groupshared float2 g_fft0[1024];
groupshared float2 g_fft1[1024];
groupshared float2 g_fft2[1024];
groupshared float2 g_fft3[1024];

float EdgeAttenuation(uint2 coordinate, uint size)
{
    const float distance = min(float(min(coordinate.y, coordinate.x)),
                               float(min((size - 1) - coordinate.y, (size - 1) - coordinate.x)));
    const float border = float(size) * 0.1f;
    const bool inner = border >= distance;
    const float normalizedDistance = (border - distance) / border;
    return inner
        ? exp2(-normalizedDistance * normalizedDistance * g_dT * 7.21347522735595703125f)
        : 1.0f;
}

[numthreads(1024, 1, 1)]
void ReverseFFTStep2ColumnsAndWriteDisplacementsForLocalWavesCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint i = dispatchThreadID.x;
    const uint size = g_simulationGridSize;
    const uint index = i << 1;
    const uint halfSize = size >> 1;

    float2 d0[2];
    float2 d1[2];
    float2 d2[2];
    float2 d3[2];

    const bool inRange = index < size;
    if (inRange)
    {
        const uint j = reversebits(index) >> g_32minusLog2Resolution;
        const uint indexA = j * size + dispatchThreadID.y;
        const uint indexB = (halfSize + j) * size + dispatchThreadID.y;
        const float4 a = g_input1[indexA];
        d0[0] = a.xy;
        const float4 b = g_input1[indexB];
        d0[1] = b.xy;
        d1[0] = a.zw;
        d1[1] = b.zw;
        const float4 c = g_input2[indexA];
        d2[0] = c.xy;
        const float4 d = g_input2[indexB];
        d2[1] = d.xy;
        d3[0] = c.zw;
        d3[1] = d.zw;
    }

    float2 u0[2] = { d0[0], d0[1] };
    float2 u1[2] = { d1[0], d1[1] };
    float2 u2[2] = { d2[0], d2[1] };
    float2 u3[2] = { d3[0], d3[1] };

    bool flag = false;
    float angleScale = 1.57079632679489661923f;

    if (i < halfSize)
    {
        const float2 a0 = u0[0];
        const float2 b0 = u0[1];
        u0[1] = a0 - b0;
        u0[0] = a0 + b0;
        const float2 a1 = u1[0];
        const float2 b1 = u1[1];
        u1[1] = a1 - b1;
        u1[0] = a1 + b1;
        const float2 a2 = u2[0];
        const float2 b2 = u2[1];
        u2[1] = a2 - b2;
        u2[0] = a2 + b2;
        const float2 a3 = u3[0];
        const float2 b3 = u3[1];
        u3[1] = a3 - b3;
        u3[0] = a3 + b3;

        flag = (i & 1) != 0;
        float2 s0, s1, s2, s3;
        [branch] if (flag) { s0 = u0[0]; s1 = u1[0]; s2 = u2[0]; s3 = u3[0]; }
        else               { s0 = u0[1]; s1 = u1[1]; s2 = u2[1]; s3 = u3[1]; }
        g_fft3[i] = s3;
        g_fft2[i] = s2;
        g_fft1[i] = s1;
        g_fft0[i] = s0;
        GroupMemoryBarrier();

        {
            const uint m = 1;
            const uint p = i ^ m;
            const uint k = i & m;
            [branch] if (flag) { u0[0] = g_fft0[p]; u1[0] = g_fft1[p]; u2[0] = g_fft2[p]; u3[0] = g_fft3[p]; }
            else               { u0[1] = g_fft0[p]; u1[1] = g_fft1[p]; u2[1] = g_fft2[p]; u3[1] = g_fft3[p]; }

            const float angle = (float)k * angleScale;
            float si, co;
            sincos(angle, si, co);
            const float2 t0 = float2(co * u0[1].x - si * u0[1].y, si * u0[1].x + co * u0[1].y);
            const float2 t1 = float2(co * u1[1].x - si * u1[1].y, si * u1[1].x + co * u1[1].y);
            const float2 t2 = float2(co * u2[1].x - si * u2[1].y, si * u2[1].x + co * u2[1].y);
            const float2 t3 = float2(co * u3[1].x - si * u3[1].y, si * u3[1].x + co * u3[1].y);
            const float2 c0 = u0[0];
            u0[1] = c0 - t0;
            u0[0] = c0 + t0;
            const float2 c1 = u1[0];
            u1[1] = c1 - t1;
            u1[0] = c1 + t1;
            const float2 c2 = u2[0];
            u2[1] = c2 - t2;
            u2[0] = c2 + t2;
            const float2 c3 = u3[0];
            u3[1] = c3 - t3;
            u3[0] = c3 + t3;

            flag = (i & 2) != 0;
            float2 w0, w1, w2, w3;
            [branch] if (flag) { w0 = u0[0]; w1 = u1[0]; w2 = u2[0]; w3 = u3[0]; }
            else               { w0 = u0[1]; w1 = u1[1]; w2 = u2[1]; w3 = u3[1]; }
            g_fft3[p] = w3;
            g_fft2[p] = w2;
            g_fft1[p] = w1;
            g_fft0[p] = w0;
            GroupMemoryBarrier();
        }
        angleScale *= 0.5f;
        {
            const uint m = 3;
            const uint p = i ^ m;
            const uint k = i & m;
            [branch] if (flag) { u0[0] = g_fft0[p]; u1[0] = g_fft1[p]; u2[0] = g_fft2[p]; u3[0] = g_fft3[p]; }
            else               { u0[1] = g_fft0[p]; u1[1] = g_fft1[p]; u2[1] = g_fft2[p]; u3[1] = g_fft3[p]; }

            const float angle = (float)k * angleScale;
            float si, co;
            sincos(angle, si, co);
            const float2 t0 = float2(co * u0[1].x - si * u0[1].y, si * u0[1].x + co * u0[1].y);
            const float2 t1 = float2(co * u1[1].x - si * u1[1].y, si * u1[1].x + co * u1[1].y);
            const float2 t2 = float2(co * u2[1].x - si * u2[1].y, si * u2[1].x + co * u2[1].y);
            const float2 t3 = float2(co * u3[1].x - si * u3[1].y, si * u3[1].x + co * u3[1].y);
            const float2 c0 = u0[0];
            u0[1] = c0 - t0;
            u0[0] = c0 + t0;
            const float2 c1 = u1[0];
            u1[1] = c1 - t1;
            u1[0] = c1 + t1;
            const float2 c2 = u2[0];
            u2[1] = c2 - t2;
            u2[0] = c2 + t2;
            const float2 c3 = u3[0];
            u3[1] = c3 - t3;
            u3[0] = c3 + t3;

            flag = (i & 4) != 0;
            float2 w3, w2, w1, w0;
            [branch] if (flag) { w0 = u0[0]; w1 = u1[0]; w2 = u2[0]; w3 = u3[0]; }
            else               { w0 = u0[1]; w1 = u1[1]; w2 = u2[1]; w3 = u3[1]; }
            g_fft3[p] = w3;
            g_fft2[p] = w2;
            g_fft1[p] = w1;
            g_fft0[p] = w0;
            GroupMemoryBarrier();
        }
        angleScale *= 0.5f;
    }

    [loop] for (uint s = 8; s < size; )
    {
        if (i < halfSize)
        {
            const uint m = s - 1;
            const uint p = i ^ m;
            const uint k = i & m;
            [branch] if (flag) { u0[0] = g_fft0[p]; u1[0] = g_fft1[p]; u2[0] = g_fft2[p]; u3[0] = g_fft3[p]; }
            else               { u0[1] = g_fft0[p]; u1[1] = g_fft1[p]; u2[1] = g_fft2[p]; u3[1] = g_fft3[p]; }

            const float angle = (float)k * angleScale;
            float si, co;
            sincos(angle, si, co);
            const float2 t0 = float2(co * u0[1].x - si * u0[1].y, si * u0[1].x + co * u0[1].y);
            const float2 t1 = float2(co * u1[1].x - si * u1[1].y, si * u1[1].x + co * u1[1].y);
            const float2 t2 = float2(co * u2[1].x - si * u2[1].y, si * u2[1].x + co * u2[1].y);
            const float2 t3 = float2(co * u3[1].x - si * u3[1].y, si * u3[1].x + co * u3[1].y);
            u0[1] = u0[0] - t0;
            u0[0] = u0[0] + t0;
            u1[1] = u1[0] - t1;
            u1[0] = u1[0] + t1;
            u2[1] = u2[0] - t2;
            u2[0] = u2[0] + t2;
            u3[1] = u3[0] - t3;
            u3[0] = u3[0] + t3;

            flag = (i & s) != 0;
            float2 w0, w1, w2, w3;
            [branch] if (flag) { w0 = u0[0]; w1 = u1[0]; w2 = u2[0]; w3 = u3[0]; }
            else               { w0 = u0[1]; w1 = u1[1]; w2 = u2[1]; w3 = u3[1]; }
            g_fft3[p] = w3;
            g_fft2[p] = w2;
            g_fft1[p] = w1;
            g_fft0[p] = w0;
        }
        GroupMemoryBarrierWithGroupSync();
        s <<= 1;
        angleScale *= 0.5f;
    }

    if (inRange)
    {
        float4 vertical0, vertical1, lateral0, lateral1;
        vertical0.xy = u0[0];
        vertical1.xy = u0[1];
        vertical0.zw = u1[0];
        vertical1.zw = u1[1];
        lateral0.xy = u2[0];
        lateral1.xy = u2[1];
        lateral0.zw = u3[0];
        lateral1.zw = u3[1];

        const uint i2 = i + halfSize;

        const float attenuation0 = EdgeAttenuation(uint2(i, dispatchThreadID.y), size);
        vertical0.x *= attenuation0;
        lateral0.x *= attenuation0;
        lateral0.z *= attenuation0;

        const float attenuation1 = EdgeAttenuation(uint2(i2, dispatchThreadID.y), size);
        vertical1.x *= attenuation1;
        lateral1.x *= attenuation1;
        lateral1.z *= attenuation1;

        const uint outIndex = size * i + dispatchThreadID.y;
        const uint outIndex2 = size * i2 + dispatchThreadID.y;
        g_output1[outIndex] = vertical0;
        g_output1[outIndex2] = vertical1;
        g_output2[outIndex] = lateral0;
        g_output2[outIndex2] = lateral1;

        const float4 displacement0 = float4(
            lateral0.z * g_lateralScaler,
            lateral0.x * g_lateralScaler,
            vertical0.x * g_verticalScaler,
            0.0f);
        const float4 displacement1 = float4(
            lateral1.z * g_lateralScaler,
            lateral1.x * g_lateralScaler,
            vertical1.x * g_verticalScaler,
            0.0f);
        g_displacementsOutput[dispatchThreadID.xy] = displacement0;
        g_displacementsOutput[uint2(i2, dispatchThreadID.y)] = displacement1;
    }
}

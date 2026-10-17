cbuffer CS_CBUFFER : register(b0)
{
    uint g_simulationGridSize : packoffset(c0.x);
    uint g_32minusLog2Resolution : packoffset(c0.y);
    float g_simulationWorldspaceSize : packoffset(c0.z);
    float g_dT : packoffset(c0.w);
    float g_dampening : packoffset(c1.x);
    uint g_numDisturbanceSamples : packoffset(c1.y);
    float g_verticalScaler : packoffset(c1.z);
    float g_lateralScaler : packoffset(c1.w);
};

StructuredBuffer<float4> g_input1 : register(t0);
RWStructuredBuffer<float4> g_output1 : register(u0);

groupshared float2 g_fft0[1024];
groupshared float2 g_fft1[1024];

[numthreads(1024, 1, 1)]
void ForwardFFTStep2ColumnsForLocalWavesCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint i = dispatchThreadID.x;
    const uint size = g_simulationGridSize;
    const uint index = i << 1;
    const uint halfSize = size >> 1;

    const uint j = reversebits(index) >> g_32minusLog2Resolution;
    const uint indexA = j * size + dispatchThreadID.y;
    const uint indexB = (halfSize + j) * size + dispatchThreadID.y;
    const float4 a = g_input1[indexA];
    const float4 b = g_input1[indexB];

    float2 u0[2];
    float2 u1[2];
    u0[0] = a.xy;
    u0[1] = b.xy;
    u1[0] = a.zw;
    u1[1] = b.zw;

    bool flag = false;
    float angleScale = 1.57079632679489661923f;

    if (i < halfSize)
    {
        const float4 dif = a - b;
        u0[1] = dif.xy;
        const float4 sum = a + b;
        u0[0] = sum.xy;
        u1[1] = dif.zw;
        u1[0] = sum.zw;

        flag = (i & 1) != 0;
        float2 s0, s1;
        [branch] if (flag) { s0 = u0[0]; s1 = u1[0]; }
        else               { s0 = u0[1]; s1 = u1[1]; }
        g_fft1[i] = s1;
        g_fft0[i] = s0;
        GroupMemoryBarrier();

        {
            const uint m = 1;
            const uint p = i ^ m;
            const uint k = i & m;
            [branch] if (flag) { u0[0] = g_fft0[p]; u1[0] = g_fft1[p]; }
            else               { u0[1] = g_fft0[p]; u1[1] = g_fft1[p]; }

            const float angle = (float)k * angleScale;
            float si, co;
            sincos(angle, si, co);
            si = -si;
            const float2 t0 = float2(co * u0[1].x - si * u0[1].y, si * u0[1].x + co * u0[1].y);
            const float2 t1 = float2(co * u1[1].x - si * u1[1].y, si * u1[1].x + co * u1[1].y);
            const float2 c0 = u0[0];
            u0[1] = c0 - t0;
            u0[0] = c0 + t0;
            const float2 c1 = u1[0];
            u1[1] = c1 - t1;
            u1[0] = c1 + t1;

            flag = (i & 2) != 0;
            float2 w0, w1;
            [branch] if (flag) { w0 = u0[0]; w1 = u1[0]; }
            else               { w0 = u0[1]; w1 = u1[1]; }
            g_fft1[p] = w1;
            g_fft0[p] = w0;
            GroupMemoryBarrier();
        }
        angleScale *= 0.5f;
        {
            const uint m = 3;
            const uint p = i ^ m;
            const uint k = i & m;
            [branch] if (flag) { u0[0] = g_fft0[p]; u1[0] = g_fft1[p]; }
            else               { u0[1] = g_fft0[p]; u1[1] = g_fft1[p]; }

            const float angle = (float)k * angleScale;
            float si, co;
            sincos(angle, si, co);
            si = -si;
            const float2 t0 = float2(co * u0[1].x - si * u0[1].y, si * u0[1].x + co * u0[1].y);
            const float2 t1 = float2(co * u1[1].x - si * u1[1].y, si * u1[1].x + co * u1[1].y);
            const float2 c0 = u0[0];
            u0[1] = c0 - t0;
            u0[0] = c0 + t0;
            const float2 c1 = u1[0];
            u1[1] = c1 - t1;
            u1[0] = c1 + t1;

            flag = (i & 4) != 0;
            float2 w1, w0;
            [branch] if (flag) { w0 = u0[0]; w1 = u1[0]; }
            else               { w0 = u0[1]; w1 = u1[1]; }
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
            [branch] if (flag) { u0[0] = g_fft0[p]; u1[0] = g_fft1[p]; }
            else               { u0[1] = g_fft0[p]; u1[1] = g_fft1[p]; }

            const float angle = (float)k * angleScale;
            float si, co;
            sincos(angle, si, co);
            si = -si;
            const float2 t0 = float2(co * u0[1].x - si * u0[1].y, si * u0[1].x + co * u0[1].y);
            const float2 t1 = float2(co * u1[1].x - si * u1[1].y, si * u1[1].x + co * u1[1].y);
            u0[1] = u0[0] - t0;
            u0[0] = u0[0] + t0;
            u1[1] = u1[0] - t1;
            u1[0] = u1[0] + t1;

            flag = (i & s) != 0;
            float2 w0, w1;
            [branch] if (flag) { w0 = u0[0]; w1 = u1[0]; }
            else               { w0 = u0[1]; w1 = u1[1]; }
            g_fft1[p] = w1;
            g_fft0[p] = w0;
        }
        GroupMemoryBarrierWithGroupSync();
        s <<= 1;
        angleScale *= 0.5f;
    }

    if (index < size)
    {
        float4 o0, o1;
        o0.xy = u0[0];
        o1.xy = u0[1];
        o0.zw = u1[0];
        o1.zw = u1[1];
        const uint outIndex = i * size + dispatchThreadID.y;
        const uint outIndex2 = (i + halfSize) * size + dispatchThreadID.y;
        g_output1[outIndex] = o0;
        g_output1[outIndex2] = o1;
    }
}

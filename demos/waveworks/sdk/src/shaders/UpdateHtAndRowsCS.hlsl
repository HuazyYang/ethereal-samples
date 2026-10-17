// Recovered from _UpdateHtAndRowsCS_bytecode_dx.msasm.

cbuffer WWCB : register(b0)
{
    uint m_resolution : packoffset(c0.x);
    uint m_resolution_plus_one : packoffset(c0.y);
    uint m_half_resolution : packoffset(c0.z);
    uint m_half_resolution_plus_one : packoffset(c0.w);
    uint m_resolution_plus_one_squared_minus_one : packoffset(c1.x);
    uint m_32_minus_log2_resolution : packoffset(c1.y);
    float m_window_in : packoffset(c1.z);
    float m_window_out : packoffset(c1.w);
    float m_frequency_scale : packoffset(c2.x);
    float m_choppy_scale : packoffset(c2.y);
    double m_time : packoffset(c2.z);
    float2 m_base_wind_dir : packoffset(c3.x);
    float m_base_wind_speed : packoffset(c3.z);
    float m_base_wind_distance : packoffset(c3.w);
    float m_base_wind_dependency : packoffset(c4.x);
    float m_base_small_waves_cutoff_length : packoffset(c4.y);
    float m_base_small_waves_cutoff_power : packoffset(c4.z);
    float m_base_spectrum_peaking : packoffset(c4.w);
    float2 m_swell_wind_dir : packoffset(c5.x);
    float m_base_linear_scale : packoffset(c5.z);
    float m_swell_wind_speed : packoffset(c5.w);
    float m_swell_wind_distance : packoffset(c6.x);
    float m_swell_wind_dependency : packoffset(c6.y);
    float m_swell_small_waves_cutoff_length : packoffset(c6.z);
    float m_swell_small_waves_cutoff_power : packoffset(c6.w);
    float m_swell_spectrum_peaking : packoffset(c7.x);
    float m_swell_linear_scale : packoffset(c7.y);
    float pad0 : packoffset(c7.z);
    float pad1 : packoffset(c7.w);
};

StructuredBuffer<float2> g_h0_input : register(t0);
StructuredBuffer<float2> g_omega_input : register(t1);
RWStructuredBuffer<float2> g_ht_output : register(u0);
RWStructuredBuffer<float4> g_dt_output : register(u1);

groupshared float2 g_height[256];
groupshared float2 g_xDisplacement[256];
groupshared float2 g_yDisplacement[256];

void Fft(uint thread, inout float2 h[2], inout float2 dx[2], inout float2 dy[2])
{
    bool upperLane = false;
    float angleScale = 1.57079637050628662109f;

    if (thread < m_half_resolution)
    {
        float2 h0 = h[0];
        float2 h1 = h[1];
        float2 x0 = dx[0];
        float2 x1 = dx[1];
        float2 y0 = dy[0];
        float2 y1 = dy[1];
        h[1] = h0 - h1;
        h[0] = h0 + h1;
        dx[1] = x0 - x1;
        dx[0] = x0 + x1;
        dy[1] = y0 - y1;
        dy[0] = y0 + y1;

        upperLane = (thread & 1u) != 0u;
        if (upperLane)
        {
            g_height[thread] = h[0];
            g_xDisplacement[thread] = dx[0];
            g_yDisplacement[thread] = dy[0];
        }
        else
        {
            g_height[thread] = h[1];
            g_xDisplacement[thread] = dx[1];
            g_yDisplacement[thread] = dy[1];
        }
        // The width 2 and 4 exchanges only ever touch a partner within eight
        // lanes, so the shipped blob settles for a plain memory barrier
        // (sync_g) inside the divergent branch rather than a group sync; that
        // is why these two stages are peeled out of the loop below. Promoting
        // them to GroupMemoryBarrierWithGroupSync is rejected by FXC (X3663).
        GroupMemoryBarrier();

        [unroll]
        for (uint width = 2u; width < 8u; width <<= 1u)
        {
            uint partner = thread ^ (width - 1u);
            uint phase = thread & (width - 1u);
            uint nextMask = thread & width;
            if (upperLane)
            {
                h[0] = g_height[partner];
                dx[0] = g_xDisplacement[partner];
                dy[0] = g_yDisplacement[partner];
            }
            else
            {
                h[1] = g_height[partner];
                dx[1] = g_xDisplacement[partner];
                dy[1] = g_yDisplacement[partner];
            }

            float angle = float(phase) * angleScale;
            float sine;
            float cosine;
            sincos(angle, sine, cosine);

            float2 rotH = float2(cosine * h[1].x - sine * h[1].y,
                                 sine * h[1].x + cosine * h[1].y);
            float2 rotX = float2(cosine * dx[1].x - sine * dx[1].y,
                                 sine * dx[1].x + cosine * dx[1].y);
            float2 rotY = float2(cosine * dy[1].x - sine * dy[1].y,
                                 sine * dy[1].x + cosine * dy[1].y);

            float2 h0 = h[0];
            h[1] = h0 - rotH;
            h[0] = h0 + rotH;
            float2 x0 = dx[0];
            dx[1] = x0 - rotX;
            dx[0] = x0 + rotX;
            float2 y0 = dy[0];
            dy[1] = y0 - rotY;
            dy[0] = y0 + rotY;

            upperLane = nextMask != 0u;
            if (upperLane)
            {
                g_height[partner] = h[0];
                g_xDisplacement[partner] = dx[0];
                g_yDisplacement[partner] = dy[0];
            }
            else
            {
                g_height[partner] = h[1];
                g_xDisplacement[partner] = dx[1];
                g_yDisplacement[partner] = dy[1];
            }
            GroupMemoryBarrier();
            angleScale *= 0.5f;
        }
    }

    // Six guarded copies: FXC cannot bound this loop, and the shipped blob has
    // the matching per-copy "ult width, m_resolution" gates chained by and.
    [unroll(6)]
    for (uint w = 8u; w < m_resolution; w <<= 1u)
    {
        if (thread < m_half_resolution)
        {
            uint partner = thread ^ (w - 1u);
            uint phase = thread & (w - 1u);
            uint nextMask = thread & w;
            if (upperLane)
            {
                h[0] = g_height[partner];
                dx[0] = g_xDisplacement[partner];
                dy[0] = g_yDisplacement[partner];
            }
            else
            {
                h[1] = g_height[partner];
                dx[1] = g_xDisplacement[partner];
                dy[1] = g_yDisplacement[partner];
            }

            float angle = float(phase) * angleScale;
            float sine;
            float cosine;
            sincos(angle, sine, cosine);

            float2 rotH = float2(cosine * h[1].x - sine * h[1].y,
                                 sine * h[1].x + cosine * h[1].y);
            float2 rotX = float2(cosine * dx[1].x - sine * dx[1].y,
                                 sine * dx[1].x + cosine * dx[1].y);
            float2 rotY = float2(cosine * dy[1].x - sine * dy[1].y,
                                 sine * dy[1].x + cosine * dy[1].y);

            float2 h0 = h[0];
            h[1] = h0 - rotH;
            h[0] = h0 + rotH;
            float2 x0 = dx[0];
            dx[1] = x0 - rotX;
            dx[0] = x0 + rotX;
            float2 y0 = dy[0];
            dy[1] = y0 - rotY;
            dy[0] = y0 + rotY;

            upperLane = nextMask != 0u;
            if (upperLane)
            {
                g_height[partner] = h[0];
                g_xDisplacement[partner] = dx[0];
                g_yDisplacement[partner] = dy[0];
            }
            else
            {
                g_height[partner] = h[1];
                g_xDisplacement[partner] = dx[1];
                g_yDisplacement[partner] = dy[1];
            }
        }
        GroupMemoryBarrierWithGroupSync();
        angleScale *= 0.5f;
    }
}

[numthreads(256, 1, 1)]
void UpdateHtAndRowsCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint thread = dispatchThreadID.x;
    uint row = dispatchThreadID.y;
    uint doubledThread = thread << 1u;

    float2 h[2];
    float2 dx[2];
    float2 dy[2];

    bool inRange = doubledThread < m_resolution;
    if (inRange)
    {
        uint firstFrequency = reversebits(doubledThread) >> m_32_minus_log2_resolution;
        int centeredX = int(firstFrequency) - int(m_half_resolution);
        int centeredY = int(row) - int(m_half_resolution);

        uint firstSpectrumIndex = row * m_resolution_plus_one + firstFrequency;
        uint firstOmegaIndex = row * m_half_resolution_plus_one + firstFrequency;
        uint secondSpectrumIndex = firstSpectrumIndex + m_half_resolution;
        uint mirroredOmegaBase =
            row * m_half_resolution_plus_one + m_half_resolution;

        float2 h0First = g_h0_input[firstSpectrumIndex];
        float2 mirroredFirst =
            g_h0_input[m_resolution_plus_one_squared_minus_one - firstSpectrumIndex];
        double2 phase;
        phase.x = double(g_omega_input[firstOmegaIndex].x) * m_time;

        float2 h0Second = g_h0_input[secondSpectrumIndex];
        float2 mirroredSecond =
            g_h0_input[m_resolution_plus_one_squared_minus_one - secondSpectrumIndex];
        phase.y =
            double(g_omega_input[mirroredOmegaBase - firstFrequency].x) * m_time;

        float2 completedTurns = floor(float2(phase * 0.15915493667125701904));
        phase -= double2(completedTurns) * 6.28318548202514648438;

        float2 sine;
        float2 cosine;
        sincos(float2(phase), sine, cosine);

        h[0].x = (h0First.x + mirroredFirst.x) * cosine.x -
            (h0First.y + mirroredFirst.y) * sine.x;
        h[1].x = (h0Second.x + mirroredSecond.x) * cosine.y -
            (h0Second.y + mirroredSecond.y) * sine.y;
        h[0].y = (h0First.x - mirroredFirst.x) * sine.x +
            (h0First.y - mirroredFirst.y) * cosine.x;
        h[1].y = (h0Second.x - mirroredSecond.x) * sine.y +
            (h0Second.y - mirroredSecond.y) * cosine.y;

        int2 waveX = int2(centeredX, int(firstFrequency));
        // The shipped blob tests the wave vector for zero with an integer or
        // (or r2.xy, r0.xxxx, r1.xyxx) and squares it in integer as well
        // (imul/imad), not through a float dot product.
        int2 nonZeroFrequency = waveX | centeredY;
        int2 lengthSquared = waveX * waveX + centeredY * centeredY;
        // Spelled per component: a vector condition on a ternary is legal under
        // FXC but rejected by DXC 2021 (it wants select()), and this same source
        // is compiled at cs_6_0 for the SPIR-V backend. FXC emits identical bytecode.
        float2 rsqrtLength = rsqrt(float2(lengthSquared));
        float2 inverseLength;
        inverseLength.x = nonZeroFrequency.x != 0 ? rsqrtLength.x : 0.0f;
        inverseLength.y = nonZeroFrequency.y != 0 ? rsqrtLength.y : 0.0f;

        // mul r1.zw, r2.xxxx, r6.xxxy then mul r2.xy, r1.zwzz, r1.xxxx: the
        // rotated height is scaled by the inverse length first and by the
        // wave-vector component second. Folding the two scalars together
        // changes the rounding.
        float2 rotated0 = float2(-h[0].y, h[0].x) * inverseLength.x;
        float2 rotated1 = float2(-h[1].y, h[1].x) * inverseLength.y;
        dx[0] = float(waveX.x) * rotated0;
        dx[1] = float(waveX.y) * rotated1;
        dy[0] = float(centeredY) * rotated0;
        dy[1] = float(centeredY) * rotated1;
    }

    Fft(thread, h, dx, dy);

    if (inRange)
    {
        float2 height0 = h[0];
        float2 height1 = h[1];
        float2 xDisplacement0 = dx[0];
        float2 xDisplacement1 = dx[1];
        float2 yDisplacement0 = dy[0];
        float2 yDisplacement1 = dy[1];

        uint firstOutput = row * m_resolution + thread;
        g_ht_output[firstOutput] = height0;
        uint secondOutput = firstOutput + m_half_resolution;
        g_ht_output[secondOutput] = height1;
        g_dt_output[firstOutput] = float4(xDisplacement0, yDisplacement0);
        g_dt_output[secondOutput] = float4(xDisplacement1, yDisplacement1);
    }
}

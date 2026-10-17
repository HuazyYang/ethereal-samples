// Recovered from _UpdateColumnsAndWriteDisplacementsCS_bytecode_dx.msasm.

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

StructuredBuffer<float2> g_ht_input : register(t0);
StructuredBuffer<float4> g_dt_input : register(t1);
RWTexture2D<float4> g_displacement_output : register(u0);

groupshared float2 g_height[256];
groupshared float2 g_xDisplacement[256];
groupshared float2 g_yDisplacement[256];

[numthreads(256, 1, 1)]
void UpdateColumnsAndWriteDisplacementsCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint thread = dispatchThreadID.x;
    uint column = dispatchThreadID.y;
    uint doubledThread = thread << 1u;
    uint firstRow = reversebits(doubledThread) >> m_32_minus_log2_resolution;
    uint firstInput = firstRow * m_resolution + column;
    uint secondRow = m_half_resolution - firstRow;
    uint secondInput = secondRow * m_resolution + column;
    bool inRange = doubledThread < m_resolution;

    float2 firstHeight = g_ht_input[firstInput];
    float2 secondHeight = g_ht_input[secondInput];
    if (inRange)
        secondHeight.y = -secondHeight.y;

    float4 firstDisplacement = g_dt_input[firstInput];
    float4 secondRaw = g_dt_input[secondInput];

    float2 h[2];
    float2 dx[2];
    float2 dy[2];
    h[0] = firstHeight;
    h[1] = secondHeight;
    dx[0] = firstDisplacement.xy;
    float4 secondDisplacement = secondRaw * float4(1.0f, -1.0f, 1.0f, -1.0f);
    dx[1] = secondDisplacement.xy;
    dy[0] = firstDisplacement.zw;
    dy[1] = secondDisplacement.zw;


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

    if (inRange)
    {
        float height0 = h[0].x;
        float height1 = h[1].x;
        float xDisplacement0 = dx[0].x;
        float xDisplacement1 = dx[1].x;
        float yDisplacement0 = dy[0].x;
        float yDisplacement1 = dy[1].x;

        float sign = ((thread + column) & 1u) != 0u ? -1.0f : 1.0f;
        float choppy = m_choppy_scale * sign;

        g_displacement_output[uint2(column, thread)] = float4(
            xDisplacement0 * choppy,
            yDisplacement0 * choppy,
            height0 * sign,
            0.0f);
        g_displacement_output[uint2(column, thread + m_half_resolution)] = float4(
            xDisplacement1 * choppy,
            yDisplacement1 * choppy,
            height1 * sign,
            0.0f);
    }
}

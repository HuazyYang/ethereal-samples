// SPDX-License-Identifier: Apache-2.0
#include "GVDBRaycastCommon.slang"
[shader("compute")]
[numthreads(8, 8, 1)]
void raycastVolume(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_Parameters.image.xy)) return;
    uint pixel = id.y * g_Parameters.image.x + id.x;
    float3 origin = g_Parameters.rayOrigin.xyz;
    float3 direction = pixelDirection(id.xy);
    float enter, leave;
    float transmittance = 1;
    float3 color = float3(0);
    float depth = asfloat(0x7f800000u);
    if (intersectVolume(origin, direction, enter, leave)) {
        float t = enter;
        for (uint step = 0; step < g_Parameters.image.w && t < leave; ++step) {
            float distance = min(g_Parameters.march.x, leave - t);
            float density = sampleVolume(origin + direction * (t + distance * 0.5));
            float4 transfer = transferValue(density);
            float opacity = 1 - exp(-max(0.0, transfer.w) * g_Parameters.march.w * distance);
            if (opacity > 0 && isinf(depth)) depth = t;
            color += transmittance * opacity * transfer.xyz;
            transmittance *= 1 - opacity;
            t += distance;
            if (1 - transmittance >= g_Parameters.valueRange.w) break;
        }
    }
    g_Output[pixel] = float4(color + transmittance * g_Parameters.background.xyz,
        1 - transmittance + transmittance * g_Parameters.background.w);
    g_Depth[pixel] = depth;
}
[shader("compute")]
[numthreads(8, 8, 1)]
void raycastSurface(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_Parameters.image.xy)) return;
    uint pixel = id.y * g_Parameters.image.x + id.x;
    float3 origin = g_Parameters.rayOrigin.xyz;
    float3 direction = pixelDirection(id.xy);
    float enter, leave;
    float4 color = g_Parameters.background;
    float depth = asfloat(0x7f800000u);
    if (intersectVolume(origin, direction, enter, leave)) {
        float previousT = enter;
        float previous = sampleVolume(origin + direction * enter) - g_Parameters.valueRange.z;
        for (uint step = 0; step < g_Parameters.image.w; ++step) {
            float t = min(enter + float(step + 1) * g_Parameters.march.x, leave);
            float value = sampleVolume(origin + direction * t) - g_Parameters.valueRange.z;
            if ((previous <= 0 && value >= 0) || (previous >= 0 && value <= 0)) {
                float low = previousT;
                float high = t;
                for (uint refine = 0; refine < 8; ++refine) {
                    float mid = (low + high) * 0.5;
                    float sampled = sampleVolume(origin + direction * mid) - g_Parameters.valueRange.z;
                    if ((previous <= 0 && sampled <= 0) || (previous >= 0 && sampled >= 0)) low = mid; else high = mid;
                }
                depth = (low + high) * 0.5;
                float3 hit = origin + direction * depth;
                float3 gradient = float3(sampleVolume(hit + float3(0.5,0,0)) - sampleVolume(hit - float3(0.5,0,0)),
                    sampleVolume(hit + float3(0,0.5,0)) - sampleVolume(hit - float3(0,0.5,0)),
                    sampleVolume(hit + float3(0,0,0.5)) - sampleVolume(hit - float3(0,0,0.5)));
                float3 normal = dot(gradient,gradient) > 1e-12 ? normalize(-gradient) : -direction;
                float diffuse = max(0.0, dot(normal, normalize(g_Parameters.light.xyz - hit)));
                float illumination = g_Parameters.light.w + (1 - g_Parameters.light.w) * diffuse;
                color = float4(transferValue(g_Parameters.valueRange.z).xyz * illumination, 1);
                break;
            }
            if (t >= leave) break;
            previousT = t;
            previous = value;
        }
    }
    g_Output[pixel] = color;
    g_Depth[pixel] = depth;
}

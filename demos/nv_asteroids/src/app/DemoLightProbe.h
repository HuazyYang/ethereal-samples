#pragma once

// Asteroids.exe: DemoLightProbe (RTTI std::_Ref_count_obj<DemoLightProbe>, object 216 bytes, created by
// 0x14002B920). The first 176 bytes are the 2018 framework LightProbe, which has the same fields as donut's
// (name, diffuse/specular/BRDF textures, array indices, scales, enabled, bounds frustum).
// SceneLoaded (0x140036540) writes the extra fields for the three probes:
//   probe 0: capture (10000, 5000, 10000), heights [1000, 50000],    tint (1, 0, 0, 0)
//   probe 1: capture (10000, -1000, 10000), heights [-5000, 1000],   tint (0, 0, 1, 0)
//   probe 2: capture (10000, -9000, 10000), heights [-50000, -5000], tint (0, 1, 0, 0)
// CollectLightProbes (0x1400377C0) turns the height band into soft bounds; the probe render (0x1400327F0)
// captures the cubemap at the capture position.

#include <donut/engine/SceneTypes.h>

struct DemoLightProbe : public donut::engine::LightProbe
{
    // Adds no interface and no class ID of its own.
    NVRHI_INHERIT_INTERFACE_TABLE()

    dm::float3 capturePosition = 0.f;   // +176 (world space)
    float minHeight = 0.f;              // +188
    float maxHeight = 0.f;              // +192
    dm::float4 debugColor = 0.f;        // +196 unresolved: never read by the recovered code (probe tint?)
};

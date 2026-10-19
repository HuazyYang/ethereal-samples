// The five original spray display modes, using explicit sparse buffers.
// SPDX-License-Identifier: Apache-2.0
#include "../../gvdb/slang/GVDBTypes.slang"
struct RenderParameters {
    VolumeInfo volume;
    uint4 image;
    float4 rayOrigin, direction00, directionDx, directionDy;
    float4 march, valueRange, background, light;
    uint4 counts;
    float4 sampling;
    uint4 mode;  // mode.x: Off,Voxel,Trilinear,Section,Volume
};
ConstantBuffer<RenderParameters> g_Parameters : register(b0);
ByteAddressBuffer g_Atlas : register(t0);
ByteAddressBuffer g_Nodes : register(t1);
ByteAddressBuffer g_Children : register(t2);
StructuredBuffer<LevelInfo> g_Levels : register(t3);
StructuredBuffer<float4> g_Transfer : register(t4);
ByteAddressBuffer g_Color : register(t5);
RWStructuredBuffer<float4> g_Output : register(u0);
RWStructuredBuffer<float> g_Depth : register(u1);
#define GVDB_RAY_QUERY_RAW 1
#include "../../gvdb/slang/GVDBRayQueryTrace.slang"
float4 colorAt(float3 atlas) {
    uint3 p = uint3(clamp(int3(atlas), int3(0), int3(g_Parameters.volume.atlasResolutionAndApron.xyz) - 1));
    uint color = g_Color.Load(flatten3(p, g_Parameters.volume.atlasResolutionAndApron.xyz) * 4);
    return float4(color & 255, (color >> 8) & 255, (color >> 16) & 255, color >> 24) / 255.0;
}
float4 transfer(float value) { return g_Transfer[uint(saturate(value) * 16300.0)]; }
TraceResult traceVoxel(float3 origin, float3 direction) {
    TraceResult result = (TraceResult)0;
    result.hit = float4(1e10, 1e10, 1e10, 0);
    float enter = 0, leave = 0;
    if (!rayBox(origin, direction, enter, leave))
        return result;
    float width = float(g_Levels[0].nodeExtent.x);
    LevelInfo rootLevel = g_Levels[g_Parameters.volume.hierarchy.x];
    float3 gridOrigin = float3(
        loadNode(g_Nodes, rootLevel.nodes.x + g_Parameters.volume.hierarchy.y * rootLevel.nodes.y).position);
    int3 leafCell = enteredCell(origin, direction, enter, gridOrigin, width);
    float t = enter;
    uint corrections = 0;
    [loop] for (uint visit = 0; visit < g_Parameters.counts.z;) {
        if (t >= leave)
            return result;
        float3 leafTimes = cellExitTimes(leafCell, gridOrigin, width, origin, direction);
        float exitT = min(leave, min(leafTimes.x, min(leafTimes.y, leafTimes.z)));
        if (exitT <= t) {
            if (++corrections > 3) {
                result.hit.w = -1;
                return result;
            }
            advanceCell(leafCell, leafTimes, exitT, direction);
            continue;
        }
        ++visit;
        float3 lower = gridOrigin + float3(leafCell) * width;
        VDBNode leaf = (VDBNode)0;
        uint leafIndex = 0;
        bool found =
            findLeaf(g_Nodes, g_Children, g_Levels, g_Parameters.volume, lower + width * .5, leaf, leafIndex);
        if (found) {
            // PrepareLeaf in the retained renderer floors the hierarchy-biased
            // position. A thin first voxel crossed by that bias is not tested.
            float sampleT = leafSamplingEntry(leaf, t, enter, origin, direction);
            int3 voxel = int3(floor(origin + sampleT * direction));
            [loop] for (uint i = 0; i < g_Parameters.counts.y; ++i) {
                if (sampleT >= exitT || any(voxel < leaf.position) || any(voxel >= leaf.position + int3(width)))
                    break;
                float3 atlas = float3(leaf.atlasPosition) + float3(voxel - leaf.position);
                if (texel(int3(atlas)) > g_Parameters.sampling.y) {
                    float voxelEntry = 0;
                    for (uint axis = 0; axis < 3; ++axis)
                        if (direction[axis] != 0) {
                            float a = (float(voxel[axis]) - origin[axis]) / direction[axis];
                            float b = (float(voxel[axis] + 1) - origin[axis]) / direction[axis];
                            voxelEntry = max(voxelEntry, min(a, b));
                        }
                    float3 hit = origin + voxelEntry * direction;
                    float3 face = hit - (float3(voxel) + .5) - .01 * direction;
                    float largest = max(abs(face.x), max(abs(face.y), abs(face.z)));
                    result.hit = float4(hit, 1);
                    result.normal = float4(float3(abs(face.x) == largest ? sign(face.x) : 0,
                                                  abs(face.y) == largest ? sign(face.y) : 0,
                                                  abs(face.z) == largest ? sign(face.z) : 0),
                                           0);
                    result.atlas = atlas;
                    return result;
                }
                float3 times = cellExitTimes(voxel, 0, 1, origin, direction);
                float next = min(exitT, min(times.x, min(times.y, times.z)));
                // PrepareLeaf may floor an exact negative-direction face into
                // a zero-length voxel. Step its face instead of reporting a limit.
                advanceCell(voxel, times, next, direction);
                sampleT = max(sampleT, next);
            }
        }
        advanceCell(leafCell, leafTimes, exitT, direction);
        t = exitT;
    }
    if (t < leave)
        result.hit.w = -1;
    return result;
}
float4 phong(TraceResult hit, float3 direction, bool voxel) {
    if (hit.hit.w <= 0)
        return g_Parameters.background;
    float3 light = normalize(g_Parameters.light.xyz - hit.hit.xyz);
    float diffuse = .9 * max(0.0, dot(hit.normal.xyz, light));
    TraceResult shadow = (TraceResult)0;
    if (voxel)
        shadow = traceVoxel(hit.hit.xyz + hit.normal.xyz, light);
    else
        shadow = traceRay(hit.hit.xyz + hit.normal.xyz, light);
    if (shadow.hit.w > 0)
        diffuse *= .2;
    return float4(colorAt(hit.atlas).xyz * (diffuse + .1), 1);
}
struct Pixel {
    float4 color;
    float depth;
};
Pixel deepPixel(float3 origin, float3 direction) {
    Pixel result = (Pixel)0;
    result.color = g_Parameters.background;
    result.depth = asfloat(0x7f800000u);
    float enter = 0, leave = 0;
    if (!rayBox(origin, direction, enter, leave))
        return result;
    float t = enter;
    float4 sum = float4(0, 0, 0, 1);
    float width = float(g_Levels[0].nodeExtent.x);
    LevelInfo rootLevel = g_Levels[g_Parameters.volume.hierarchy.x];
    float3 gridOrigin = float3(
        loadNode(g_Nodes, rootLevel.nodes.x + g_Parameters.volume.hierarchy.y * rootLevel.nodes.y).position);
    int3 cell = enteredCell(origin, direction, enter, gridOrigin, width);
    uint corrections = 0;
    [loop] for (uint visit = 0; visit < g_Parameters.counts.z;) {
        if (t >= leave || sum.w <= .01)
            break;
        float3 p = gridOrigin + (float3(cell) + .5) * width;
        float3 times = cellExitTimes(cell, gridOrigin, width, origin, direction);
        float exitT = min(leave, min(times.x, min(times.y, times.z)));
        if (exitT <= t) {
            if (++corrections > 3) {
                result.depth = -1;
                return result;
            }
            advanceCell(cell, times, exitT, direction);
            continue;
        }
        ++visit;
        VDBNode leaf = (VDBNode)0;
        uint leafIndex = 0;
        bool found = findLeaf(g_Nodes, g_Children, g_Levels, g_Parameters.volume, p, leaf, leafIndex);
        if (found) {
            float sampleT = .25 * ceil(leafSamplingEntry(leaf, t, enter, origin, direction) / .25);
            [loop] for (uint i = 0; i < 256; ++i) {
                if (sampleT >= exitT || sum.w <= .01)
                    break;
                float3 atlas =
                    float3(leaf.atlasPosition) + origin + sampleT * direction - float3(leaf.position);
                float value = atlasSample(atlas);
                if (value >= .005) {
                    float4 val = transfer(value);
                    float transmission = exp(-val.w * .25);
                    sum.xyz += val.xyz * sum.w * (1 - transmission) * 1.2 * colorAt(atlas).xyz;
                    sum.w *= transmission;
                    if (transmission < 1 && asuint(result.depth) == 0x7f800000u)
                        result.depth = sampleT;
                }
                sampleT += .25;
            }
            sum.xyz = min(sum.xyz, 1);
            sum.w = max(sum.w, 0);
        }
        advanceCell(cell, times, exitT, direction);
        t = exitT;
    }
    float alpha = 1 - sum.w;
    result.color = float4(lerp(g_Parameters.background.xyz, sum.xyz, alpha), alpha);
    return result;
}
Pixel renderPixel(float3 origin, float3 direction) {
    Pixel result = (Pixel)0;
    result.color = g_Parameters.background;
    result.depth = asfloat(0x7f800000u);
    uint mode = g_Parameters.mode.x;
    if (mode == 0) {
        result.color = 0;
        return result;
    }
    if (mode == 4)
        return deepPixel(origin, direction);
    if (mode == 3) {
        float plane = (50 - origin.x) / direction.x;
        float scalar = 0;
        float4 section = float4(1, 1, 1, 0);
        float3 start = origin;
        if (plane > 0 && (asuint(plane) & 0x7fffffffu) < 0x7f800000u) {
            start = origin + plane * direction;
            VDBNode leaf = (VDBNode)0;
            uint leafIndex = 0;
            bool found = findLeaf(g_Nodes, g_Children, g_Levels, g_Parameters.volume, start, leaf, leafIndex);
            if (found) {
                float3 atlas = float3(leaf.atlasPosition) + start - float3(leaf.position);
                scalar = atlasSample(atlas);
                section = transfer(scalar);
                section.xyz *= colorAt(atlas).xyz;
            }
        }
        TraceResult hit = traceRay(start, direction);
        if (hit.hit.w < 0) {
            result.depth = -1;
            return result;
        }
        if (hit.hit.w > 0) {
            float3 light = normalize(g_Parameters.light.xyz - hit.hit.xyz);
            float diffuse = scalar > .3 ? 1 : .8 * max(0.0, dot(hit.normal.xyz, light));
            result.color = lerp(colorAt(hit.atlas) * diffuse, section, section.w);
            result.depth = length(hit.hit.xyz - origin);
        } else {
            result.color = lerp(g_Parameters.background, section, section.w);
            if (section.w > 0)
                result.depth = plane;
        }
        result.color.w = 1;
        return result;
    }
    TraceResult hit = (TraceResult)0;
    if (mode == 1)
        hit = traceVoxel(origin, direction);
    else
        hit = traceRay(origin, direction);
    if (hit.hit.w < 0) {
        result.depth = -1;
        return result;
    }
    result.color = phong(hit, direction, mode == 1);
    if (hit.hit.w > 0)
        result.depth = length(hit.hit.xyz - origin);
    return result;
}
[shader("compute")][numthreads(8, 8, 1)] void renderSpray(uint3 id
                                                          : SV_DispatchThreadID) {
    if (any(id.xy >= g_Parameters.image.xy))
        return;
    float3 direction =
        normalize(g_Parameters.direction00.xyz + (float(id.x) + .5) * g_Parameters.directionDx.xyz +
                  (float(id.y) + .5) * g_Parameters.directionDy.xyz);
    Pixel result = renderPixel(g_Parameters.rayOrigin.xyz, direction);
    // The retained kernels write uchar4. Round toward zero before presentation.
    uint index = id.y * g_Parameters.image.x + id.x;
    g_Output[index] = float4(uint4(saturate(result.color) * 255.0)) / 255.0;
    g_Depth[index] = result.depth;
}

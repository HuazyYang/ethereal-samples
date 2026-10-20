// Retained shading equations over immutable sparse snapshots.
// SPDX-License-Identifier: Apache-2.0
#include "../../gvdb/slang/GVDBTypes.slang"
struct RenderParameters {
    VolumeInfo volume;
    uint4 image;
    float4 rayOrigin, direction00, directionDx, directionDy;
    float4 march, valueRange, background, light;
    uint4 counts;
    float4 sampling;
    uint4 mode; // Off,Voxel,Trilinear,SurfaceDepth,Deep,LevelSet,EmptySkip
    float4 lighting, integration;
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
    if (g_Parameters.mode.y == 0) return 1;
    uint3 p = uint3(clamp(int3(atlas), int3(0), int3(g_Parameters.volume.atlasResolutionAndApron.xyz) - 1));
    uint color = g_Color.Load(flatten3(p, g_Parameters.volume.atlasResolutionAndApron.xyz) * 4);
    return float4(color & 255, (color >> 8) & 255, (color >> 16) & 255, color >> 24) / 255.0;
}
float4 transfer(float value) { return g_Transfer[uint(saturate((value-g_Parameters.valueRange.z)/(g_Parameters.valueRange.y-g_Parameters.valueRange.x)) * 16300.0)]; }
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
            uint iteration=0;
            [loop] for (; iteration < g_Parameters.counts.y; ++iteration) {
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
            if (iteration==g_Parameters.counts.y && sampleT<exitT) {
                result.hit.w=-1; return result;
            }
        }
        advanceCell(leafCell, leafTimes, exitT, direction);
        t = exitT;
    }
    if (t < leave)
        result.hit.w = -1;
    return result;
}
TraceResult traceLevelOrEmpty(float3 origin, float3 direction) {
    TraceResult result;
    result.hit = float4(1e10, 1e10, 1e10, 0);
    result.normal = 0;
    result.atlas = 0;
    [branch] if (!finite3(origin) || !finite3(direction) || all(direction == 0)) return result;
    float enter = 0, leave = 0;
    [branch] if (!rayBox(origin, direction, enter, leave)) return result;
    float epsilon = g_Parameters.sampling.z, step = g_Parameters.sampling.x;
    float width = float(g_Levels[0].nodeExtent.x);
    LevelInfo rootLevel = g_Levels[g_Parameters.volume.hierarchy.x];
    float3 gridOrigin = float3(
        loadNode(g_Nodes, rootLevel.nodes.x + g_Parameters.volume.hierarchy.y * rootLevel.nodes.y).position);
    uint corrections = 0;
    float t = enter;
    int3 cell = enteredCell(origin, direction, enter, gridOrigin, width);
    [loop] for (uint visit = 0; visit < g_Parameters.counts.z;) {
        [branch] if (t >= leave) return result;
        float3 lower = float3(cell) * width + gridOrigin;
        float3 times = cellExitTimes(cell, gridOrigin, width, origin, direction);
        float exitT = min(leave, min(times.x, min(times.y, times.z)));
        // A rounded box-entry reconstruction may select the cell just before
        // the entry face. Consume such zero-length cells without sampling or
        // charging the nonempty-interval budget. At most three faces can need it.
        if (exitT <= t) {
            if (++corrections > 3) {
                result.hit.w = -1;
                return result;
            }
            advanceCell(cell, times, exitT, direction);
            continue;
        }
        ++visit;
        float3 p = lower + width * .5;
        VDBNode leaf = (VDBNode)0;
        uint leafIndex = 0;
        bool found = findLeaf(g_Nodes, g_Children, g_Levels, g_Parameters.volume, p, leaf, leafIndex);
        [branch] if (found) {
            float sampleT = leafSamplingEntry(leaf, t, enter, origin, direction);
            if (g_Parameters.mode.x==6) {
                result.hit=float4(origin+sampleT*direction,1);
                return result;
            }
            uint iteration = 0;
            [loop] for (; iteration < g_Parameters.counts.y; ++iteration) {
                [branch] if (sampleT >= exitT) break;
                float3 position = origin + sampleT * direction;
                float3 atlas = float3(leaf.atlasPosition) + position - float3(leaf.position);
                float value = atlasSample(atlas);
                [branch] if (value < g_Parameters.sampling.y) {
                    float3 normal =
                        float3(atlasSample(atlas - float3(.5, 0, 0)) - atlasSample(atlas + float3(.5, 0, 0)),
                               atlasSample(atlas - float3(0, .5, 0)) - atlasSample(atlas + float3(0, .5, 0)),
                               atlasSample(atlas - float3(0, 0, .5)) - atlasSample(atlas + float3(0, 0, .5)));
                    float norm = dot(normal, normal);
                    if (norm > 0)
                        normal *= -rsqrt(norm);
                    result.hit = float4(position - direction * g_Parameters.sampling.w, 1);
                    result.normal = float4(normal, 0);
                    result.atlas = atlas;
                    return result;
                }
                sampleT += step;
            }
            [branch] if (iteration == g_Parameters.counts.y && sampleT < exitT) {
                result.hit = float4(1e10, 1e10, 1e10, -1);
                return result;
            }
        }
        advanceCell(cell, times, exitT, direction);
        t = exitT;
    }
    if (t < leave)
        result.hit = float4(1e10, 1e10, 1e10, -1);
    return result;
}

TraceResult traceMode(float3 origin,float3 direction) {
    TraceResult result=(TraceResult)0;
    if (g_Parameters.mode.x==1) result=traceVoxel(origin,direction);
    else if (g_Parameters.mode.x>=5) result=traceLevelOrEmpty(origin,direction);
    else result=traceRay(origin,direction);
    return result;
}
float4 phong(TraceResult hit, out bool exhausted) {
    exhausted=false;
    if (hit.hit.w<=0) return g_Parameters.background;
    float3 delta=g_Parameters.light.xyz-hit.hit.xyz;
    float lengthSquared=dot(delta,delta);
    float3 light=lengthSquared>0 && isfinite(lengthSquared)?delta*rsqrt(lengthSquared):float3(0);
    float diffuse=.9*max(0,dot(hit.normal.xyz,light));
    if (g_Parameters.lighting.x>0 && diffuse>0) {
        TraceResult shadow=traceMode(hit.hit.xyz+hit.normal.xyz*g_Parameters.lighting.y,light);
        if (shadow.hit.w<0) { exhausted=true; return g_Parameters.background; }
        if (shadow.hit.w>0) diffuse*=1-g_Parameters.lighting.x;
    }
    return float4(colorAt(hit.atlas).xyz*(diffuse+.1),1);
}
struct Pixel {
    float4 color;
    float depth;
};
Pixel deepPixel(float3 origin, float3 direction) {
    Pixel result = (Pixel)0;
    result.color = float4(g_Parameters.background.xyz,0);
    result.depth = asfloat(0x7f800000u);
    float enter = 0, leave = 0;
    if (!rayBox(origin, direction, enter, leave))
        return result;
    leave=min(leave,g_Parameters.march.z);
    float t = enter;
    float4 sum = float4(0, 0, 0, 1);
    float width = float(g_Levels[0].nodeExtent.x);
    LevelInfo rootLevel = g_Levels[g_Parameters.volume.hierarchy.x];
    float3 gridOrigin = float3(
        loadNode(g_Nodes, rootLevel.nodes.x + g_Parameters.volume.hierarchy.y * rootLevel.nodes.y).position);
    int3 cell = enteredCell(origin, direction, enter, gridOrigin, width);
    uint corrections = 0;
    [loop] for (uint visit = 0; visit < g_Parameters.counts.z;) {
        if (t >= leave || sum.w <= g_Parameters.integration.y)
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
            float step=g_Parameters.sampling.x;
            float sampleT = step * ceil(leafSamplingEntry(leaf,t,enter,origin,direction)/step);
            uint iteration=0;
            [loop] for (; iteration < g_Parameters.counts.y; ++iteration) {
                if (sampleT >= exitT || sum.w <= g_Parameters.integration.y)
                    break;
                float3 atlas =
                    float3(leaf.atlasPosition) + origin + sampleT * direction - float3(leaf.position);
                float value = atlasSample(atlas);
                if (value >= g_Parameters.integration.x) {
                    float4 val = transfer(value);
                    float transmission = exp(-g_Parameters.lighting.z * val.w * step);
                    sum.xyz += val.xyz * sum.w * (1 - transmission) * g_Parameters.lighting.w * colorAt(atlas).xyz;
                    sum.w *= transmission;
                    if (transmission < 1 && asuint(result.depth) == 0x7f800000u)
                        result.depth = sampleT;
                }
                sampleT += step;
            }
            if (iteration==g_Parameters.counts.y && sampleT<exitT && sum.w>g_Parameters.integration.y) {
                result.depth=-1; return result;
            }
            sum.xyz = min(sum.xyz, 1);
            sum.w = max(sum.w, 0);
        }
        advanceCell(cell, times, exitT, direction);
        t = exitT;
    }
    if (t<leave && sum.w>g_Parameters.integration.y) { result.depth=-1; return result; }
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
    if (g_Parameters.mode.z==0 || all(direction==0)) {
        if (mode==4) result.color.w=0;
        if (mode==6) result.color.w=1;
        return result;
    }
    if (mode == 4)
        return deepPixel(origin, direction);
    TraceResult hit = traceMode(origin,direction);
    if (hit.hit.w < 0) {
        result.depth = -1;
        return result;
    }
    bool shadowExhausted=false;
    if (mode==6)
        result.color=hit.hit.w>0?float4(hit.hit.xyz*.01,1):float4(g_Parameters.background.xyz,1);
    else
        result.color=phong(hit,shadowExhausted);
    if (shadowExhausted) { result.depth=-1; return result; }
    if (hit.hit.w > 0)
        result.depth = length(hit.hit.xyz - origin);
    return result;
}
[shader("compute")][numthreads(8, 8, 1)] void renderMode(uint3 id
                                                          : SV_DispatchThreadID) {
    if (any(id.xy >= g_Parameters.image.xy))
        return;
    float3 direction =
        (g_Parameters.direction00.xyz + (float(id.x) + .5) * g_Parameters.directionDx.xyz +
                  (float(id.y) + .5) * g_Parameters.directionDy.xyz);
    float lengthSquared=dot(direction,direction);
    direction=lengthSquared>0 && isfinite(lengthSquared)?direction*rsqrt(lengthSquared):float3(0);
    Pixel result = renderPixel(g_Parameters.rayOrigin.xyz, direction);
    // The retained kernels write uchar4. Round toward zero before presentation.
    uint index = id.y * g_Parameters.image.x + id.x;
    if (g_Parameters.mode.x==6) {
        // Retained PTX converts to unsigned 32-bit, then stores the low byte.
        // Define its positive-coordinate diagnostic wrap explicitly on all APIs.
        float3 scaled=max(result.color.xyz*255.0,0);
        uint3 bytes;
        for(uint axis=0;axis<3;++axis)
            bytes[axis]=scaled[axis]>=4294967296.0?255u:(uint(scaled[axis])&255u);
        g_Output[index]=float4(float3(bytes)/255.0,1);
    } else g_Output[index] = float4(uint4(saturate(result.color) * 255.0)) / 255.0;
    g_Depth[index] = result.depth;
}

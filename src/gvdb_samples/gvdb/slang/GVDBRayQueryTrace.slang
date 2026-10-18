// Pure shared ray evaluation for arbitrary bundles and spray visualization. SPDX-License-Identifier: Apache-2.0
bool finite3(float3 v) { return all((asuint(v) & uint3(0x7fffffffu)) < uint3(0x7f800000u)); }
float texel(int3 p) {
    return g_Atlas[flatten3(uint3(clamp(p, int3(0), int3(g_Parameters.volume.atlasResolutionAndApron.xyz) - 1)),
                            g_Parameters.volume.atlasResolutionAndApron.xyz)];
}
float atlasSample(float3 p) {
    float3 q = p - .5;
    int3 lo = int3(floor(q));
    float3 w = q - float3(lo);
    return lerp(lerp(lerp(texel(lo), texel(lo + int3(1, 0, 0)), w.x),
                     lerp(texel(lo + int3(0, 1, 0)), texel(lo + int3(1, 1, 0)), w.x), w.y),
                lerp(lerp(texel(lo + int3(0, 0, 1)), texel(lo + int3(1, 0, 1)), w.x),
                     lerp(texel(lo + int3(0, 1, 1)), texel(lo + int3(1, 1, 1)), w.x), w.y),
                w.z);
}
bool rayBox(float3 origin, float3 direction, out float enter, out float leave) {
    enter = 0;
    leave = 1e30;
    for (uint axis = 0; axis < 3; ++axis) {
        float lo = g_Parameters.volume.boundsMin[axis], hi = g_Parameters.volume.boundsMax[axis];
        if (direction[axis] == 0) {
            if (origin[axis] < lo || origin[axis] >= hi)
                return false;
        } else {
            float a = (lo - origin[axis]) / direction[axis], b = (hi - origin[axis]) / direction[axis];
            enter = max(enter, min(a, b));
            leave = min(leave, max(a, b));
        }
    }
    return enter < leave;
}
// Cell selection is geometric. Sampling epsilon must not move the DDA into a
// different cell, even for unnormalized rays with very short parameter intervals.
int3 enteredCell(float3 origin, float3 direction, float t, float3 gridOrigin, float width) {
    float3 q = (origin + t * direction - gridOrigin) / width;
    float3 lower = floor(q);
    return int3(lower) - int3(direction.x < 0 && q.x == lower.x ? 1 : 0,
                              direction.y < 0 && q.y == lower.y ? 1 : 0,
                              direction.z < 0 && q.z == lower.z ? 1 : 0);
}
float cellExitTime(int cell, float gridOrigin, float width, float origin, float direction) {
    if (direction == 0)
        return 1e30;
    float face = gridOrigin + float(cell + (direction > 0 ? 1 : 0)) * width;
    return (face - origin) / direction;
}
float3 cellExitTimes(int3 cell, float3 gridOrigin, float width, float3 origin, float3 direction) {
    return float3(cellExitTime(cell.x, gridOrigin.x, width, origin.x, direction.x),
                  cellExitTime(cell.y, gridOrigin.y, width, origin.y, direction.y),
                  cellExitTime(cell.z, gridOrigin.z, width, origin.z, direction.z));
}
void advanceCell(inout int3 cell, float3 times, float exitT, float3 direction) {
    int3 delta = int3(direction.x != 0 && times.x <= exitT ? (direction.x > 0 ? 1 : -1) : 0,
                      direction.y != 0 && times.y <= exitT ? (direction.y > 0 ? 1 : -1) : 0,
                      direction.z != 0 && times.z <= exitT ? (direction.z > 0 ? 1 : -1) : 0);
    cell += delta;
}
// Legacy HDDA adds epsilon at global entry and at every descent. A leaf
// reached after crossing an ancestor boundary inherits that ancestor's bias.
// The latest of these biased entries determines the absolute sampling wavefront.
float leafSamplingEntry(VDBNode leaf, float leafEntry, float globalEntry, float3 origin, float3 direction) {
    float epsilon = g_Parameters.sampling.z;
    float result =
        max(leafEntry + epsilon, globalEntry + epsilon * float(g_Parameters.volume.hierarchy.x + 1u));
    uint2 parent = leaf.parent;
    [loop] for (uint depth = 0; depth < 10; ++depth) {
        if (nullReference(parent) || !referenceFitsIndex(parent))
            break;
        uint level = referenceLevel(parent);
        uint index = referenceIndex(parent);
        if (level >= g_Parameters.volume.hierarchy.z)
            break;
        LevelInfo info = g_Levels[level];
        if (index >= info.nodes.z)
            break;
        VDBNode node = loadNode(g_Nodes, info.nodes.x + index * info.nodes.y);
        float entry = 0;
        for (uint axis = 0; axis < 3; ++axis)
            if (direction[axis] != 0) {
                float a = (float(node.position[axis]) - origin[axis]) / direction[axis];
                float b = (float(node.position[axis] + info.nodeExtent[axis]) - origin[axis]) / direction[axis];
                entry = max(entry, min(a, b));
            }
        result = max(result, entry + epsilon * float(level + 1u));
        parent = node.parent;
    }
    return result;
}
struct TraceResult {
    float4 hit;
    float4 normal;
    float3 atlas;
};
TraceResult traceRay(float3 origin, float3 direction) {
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
            float sampleT = step * ceil(leafSamplingEntry(leaf, t, enter, origin, direction) / step);
            uint iteration = 0;
            [loop] for (; iteration < g_Parameters.counts.y; ++iteration) {
                [branch] if (sampleT >= exitT) break;
                float3 position = origin + sampleT * direction;
                float3 atlas = float3(leaf.atlasPosition) + position - float3(leaf.position);
                float value = atlasSample(atlas);
                [branch] if (value >= g_Parameters.sampling.y) {
                    float3 normal =
                        float3(atlasSample(atlas - float3(.5, 0, 0)) - atlasSample(atlas + float3(.5, 0, 0)),
                               atlasSample(atlas - float3(0, .5, 0)) - atlasSample(atlas + float3(0, .5, 0)),
                               atlasSample(atlas - float3(0, 0, .5)) - atlasSample(atlas + float3(0, 0, .5)));
                    float norm = dot(normal, normal);
                    if (norm > 0)
                        normal *= rsqrt(norm);
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

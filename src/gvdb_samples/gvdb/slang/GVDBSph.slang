// SPH fluid pool kernels using NVRHI-compatible explicit resources.
// Adapted from SPHFluidKernels.cu. SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct SphParameters {
    float4 gridMinimumAndCellWidth;
    uint4 gridResolutionAndCount;
    float4 physical;       // world smoothing radius, simulation scale, particle mass, rest density
    float4 kernels;        // poly6, spiky, viscosity term, pressure stiffness
    float4 boundaryMinimum;
    float4 boundaryMaximum;
    float4 boundary;        // particle radius, stiffness, damping, friction
    float4 integration;     // time, delta time, acceleration limit, velocity limit
    float4 gravityAndSlope;
    float4 wall;            // frequency, force, reserved, reserved
};
ConstantBuffer<SphParameters> g_Parameters : register(b0);
StructuredBuffer<float4> g_Positions : register(t0);
StructuredBuffer<float4> g_Velocities : register(t1);
StructuredBuffer<float4> g_EvaluatedVelocities : register(t2);
StructuredBuffer<uint> g_PointCells : register(t3);
StructuredBuffer<uint> g_CellCounts : register(t4);
StructuredBuffer<uint> g_CellOffsets : register(t5);
StructuredBuffer<uint> g_PointIndices : register(t6);
StructuredBuffer<float> g_Densities : register(t7);
StructuredBuffer<float4> g_Forces : register(t8);
StructuredBuffer<uint> g_PointOffsets : register(t9);
RWStructuredBuffer<float> g_OutputDensities : register(u0);
RWStructuredBuffer<float4> g_OutputForces : register(u1);
RWStructuredBuffer<float4> g_OutputPositions : register(u2);
RWStructuredBuffer<float4> g_OutputVelocities : register(u3);
RWStructuredBuffer<float4> g_OutputEvaluatedVelocities : register(u4);
RWStructuredBuffer<uint> g_OutputCells : register(u5);
RWStructuredBuffer<uint> g_OutputCellCounts : register(u6);
RWStructuredBuffer<uint> g_OutputOffsets : register(u7);

uint cellCount() { return g_Parameters.gridResolutionAndCount.x * g_Parameters.gridResolutionAndCount.y * g_Parameters.gridResolutionAndCount.z; }
uint particleCount() { return g_Parameters.gridResolutionAndCount.w; }
bool validCell(int3 cell) { return all(cell >= 0) && all(cell < int3(g_Parameters.gridResolutionAndCount.xyz)); }
[shader("compute")]
[numthreads(256, 1, 1)]
void clearGrid(uint3 id : SV_DispatchThreadID) {
    if (id.x < cellCount()) g_OutputCellCounts[id.x] = 0;
}
[shader("compute")]
[numthreads(256, 1, 1)]
void insertGrid(uint3 id : SV_DispatchThreadID) {
    if (id.x >= particleCount()) return;
    float3 position = g_Positions[id.x].xyz;
    uint cellIndex = GVDB_INVALID;
    uint offset = 0;
    if (all(isfinite(position))) {
        int3 cell = int3(floor((position - g_Parameters.gridMinimumAndCellWidth.xyz) / g_Parameters.gridMinimumAndCellWidth.w));
        if (validCell(cell)) {
            cellIndex = flatten3(uint3(cell), g_Parameters.gridResolutionAndCount.xyz);
            InterlockedAdd(g_OutputCellCounts[cellIndex], 1, offset);
        }
    }
    g_OutputCells[id.x] = cellIndex;
    g_OutputOffsets[id.x] = offset;
}
// Bind the output particle-index buffer at u5, and globally scanned grid counts
// at t5. Output cells and indices are distinct allocations between dispatches.
[shader("compute")]
[numthreads(256, 1, 1)]
void scatterGrid(uint3 id : SV_DispatchThreadID) {
    if (id.x >= particleCount()) return;
    uint cell = g_PointCells[id.x];
    if (cell < cellCount()) g_OutputCells[g_CellOffsets[cell] + g_PointOffsets[id.x]] = id.x;
}
[shader("compute")]
[numthreads(256, 1, 1)]
void computeDensity(uint3 id : SV_DispatchThreadID) {
    if (id.x >= particleCount()) return;
    float sum = 0;
    uint cellIndex = g_PointCells[id.x];
    if (cellIndex < cellCount()) {
        int3 cell = int3(unflatten3(cellIndex, g_Parameters.gridResolutionAndCount.xyz));
        float3 position = g_Positions[id.x].xyz;
        float radiusSquared = g_Parameters.physical.x * g_Parameters.physical.x;
        float scaleSquared = g_Parameters.physical.y * g_Parameters.physical.y;
        for (int z = -1; z <= 1; ++z) for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
            int3 neighbor = cell + int3(x, y, z);
            if (!validCell(neighbor)) continue;
            uint c = flatten3(uint3(neighbor), g_Parameters.gridResolutionAndCount.xyz);
            uint end = g_CellOffsets[c] + g_CellCounts[c];
            for (uint i = g_CellOffsets[c]; i < end; ++i) {
                float3 difference = position - g_Positions[g_PointIndices[i]].xyz;
                float distanceSquared = dot(difference, difference);
                if (distanceSquared > 0 && distanceSquared < radiusSquared) {
                    float weight = (radiusSquared - distanceSquared) * scaleSquared;
                    sum += weight * weight * weight;
                }
            }
        }
    }
    sum *= g_Parameters.physical.z * g_Parameters.kernels.x;
    g_OutputDensities[id.x] = sum == 0 ? 1 : sum;
}
[shader("compute")]
[numthreads(256, 1, 1)]
void computeForce(uint3 id : SV_DispatchThreadID) {
    if (id.x >= particleCount()) return;
    float3 force = float3(0);
    uint cellIndex = g_PointCells[id.x];
    if (cellIndex < cellCount()) {
        int3 cell = int3(unflatten3(cellIndex, g_Parameters.gridResolutionAndCount.xyz));
        float3 position = g_Positions[id.x].xyz;
        float radiusSquared = g_Parameters.physical.x * g_Parameters.physical.x;
        float scale = g_Parameters.physical.y;
        float physicalRadius = g_Parameters.physical.x * scale;
        float density = g_Densities[id.x];
        float pressure = (density - g_Parameters.physical.w) * g_Parameters.kernels.w;
        for (int z = -1; z <= 1; ++z) for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
            int3 neighbor = cell + int3(x, y, z);
            if (!validCell(neighbor)) continue;
            uint c = flatten3(uint3(neighbor), g_Parameters.gridResolutionAndCount.xyz);
            uint end = g_CellOffsets[c] + g_CellCounts[c];
            for (uint i = g_CellOffsets[c]; i < end; ++i) {
                uint j = g_PointIndices[i];
                float3 difference = position - g_Positions[j].xyz;
                float distanceSquared = dot(difference, difference);
                if (distanceSquared > 0 && distanceSquared < radiusSquared) {
                    float distance = sqrt(distanceSquared) * scale;
                    float neighborDensity = g_Densities[j];
                    float neighborPressure = (neighborDensity - g_Parameters.physical.w) * g_Parameters.kernels.w;
                    float radiusDifference = physicalRadius - distance;
                    float pressureTerm = scale * -0.5 * radiusDifference * g_Parameters.kernels.y * (pressure + neighborPressure) / distance;
                    force += (pressureTerm * difference + g_Parameters.kernels.z *
                        (g_EvaluatedVelocities[j].xyz - g_EvaluatedVelocities[id.x].xyz)) * radiusDifference / (density * neighborDensity);
                }
            }
        }
    }
    g_OutputForces[id.x] = float4(force, 0);
}
float3 boundaryForce(float difference, float3 normal, float3 evaluatedVelocity, float stiffness, float friction) {
    if (difference <= 1e-5) return float3(0);
    return normal * (stiffness * difference - g_Parameters.boundary.z * dot(normal, evaluatedVelocity)) - evaluatedVelocity * friction;
}
float3 limitLength(float3 v, float maximum) {
    float lengthSquared = dot(v, v);
    return lengthSquared > maximum * maximum ? v * (maximum * rsqrt(lengthSquared)) : v;
}
[shader("compute")]
[numthreads(256, 1, 1)]
void advanceParticles(uint3 id : SV_DispatchThreadID) {
    if (id.x >= particleCount()) return;
    float4 position = g_Positions[id.x];
    float3 velocity = g_Velocities[id.x].xyz;
    float3 evaluatedVelocity = g_EvaluatedVelocities[id.x].xyz;
    if (g_PointCells[id.x] >= cellCount()) {
        g_OutputPositions[id.x] = float4(-1000, -1000, -1000, position.w);
        g_OutputVelocities[id.x] = float4(0);
        g_OutputEvaluatedVelocities[id.x] = float4(0);
        return;
    }
    float3 acceleration = g_Forces[id.x].xyz * g_Parameters.physical.z;
    float radius = g_Parameters.boundary.x;
    float scale = g_Parameters.physical.y;
    float stiffness = g_Parameters.boundary.y;
    float friction = g_Parameters.boundary.w;
    float3 minimum = g_Parameters.boundaryMinimum.xyz;
    float3 maximum = g_Parameters.boundaryMaximum.xyz;
    float slope = g_Parameters.gravityAndSlope.w;
    float wave = (sin(g_Parameters.integration.x * g_Parameters.wall.x + position.z / 400.0) * 0.5 + 0.5) * g_Parameters.wall.y;
    acceleration += boundaryForce(radius - (position.y - (minimum.y + (position.x - minimum.x) * slope)) * scale, float3(-slope, 1-slope, 0), evaluatedVelocity, stiffness, friction);
    acceleration += boundaryForce(radius - (maximum.y - position.y) * scale, float3(0,-1,0), evaluatedVelocity, stiffness, friction);
    acceleration += boundaryForce(radius - (position.x - (minimum.x + wave)) * scale, float3(1,0,0), evaluatedVelocity, wave * stiffness, 0);
    acceleration += boundaryForce(radius - (maximum.x - position.x) * scale, float3(-1,0,0), evaluatedVelocity, stiffness, 0);
    acceleration += boundaryForce(radius - (position.z - minimum.z) * scale, float3(0,0,1), evaluatedVelocity, stiffness, 0);
    acceleration += boundaryForce(radius - (maximum.z - position.z) * scale, float3(0,0,-1), evaluatedVelocity, stiffness, 0);
    acceleration += g_Parameters.gravityAndSlope.xyz;
    acceleration = limitLength(acceleration, g_Parameters.integration.z);
    velocity = limitLength(velocity, g_Parameters.integration.w);
    float3 nextVelocity = velocity + acceleration * g_Parameters.integration.y;
    position.xyz += nextVelocity * (g_Parameters.integration.y / scale);
    g_OutputPositions[id.x] = position;
    g_OutputVelocities[id.x] = float4(nextVelocity, 0);
    g_OutputEvaluatedVelocities[id.x] = float4((velocity + nextVelocity) * 0.5, 0);
}

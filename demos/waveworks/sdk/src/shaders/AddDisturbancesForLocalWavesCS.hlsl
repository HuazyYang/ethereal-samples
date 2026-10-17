// Recovered from the matching DXBC assembly; low-level register flow is retained.

cbuffer CS_CBUFFER : register(b0)
{
    uint g_simulationGridSize : packoffset(c0.x);
    uint g_32minusLog2Resolution : packoffset(c0.y);
    float g_simulationWorldspaceSize : packoffset(c0.z);
    float g_dT : packoffset(c0.w);
    float g_dampening : packoffset(c1.x);
    uint g_numDisturbanceSamples : packoffset(c1.y);
    float2 pad0 : packoffset(c1.z);
};

StructuredBuffer<float4> g_disturbances : register(t0);
RWStructuredBuffer<float4> g_simulationData : register(u0);

[numthreads(128, 1, 1)]
void AddDisturbancesForLocalWavesCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint disturbanceIndex = dispatchThreadID.x;
    if (disturbanceIndex < g_numDisturbanceSamples)
    {
        float4 disturbance = g_disturbances[disturbanceIndex];
        int2 cell = int2(disturbance.xy);
        uint simulationIndex = cell.y * g_simulationGridSize + cell.x;

        float displacementInput = g_simulationData[simulationIndex].x;
        float velocityPotentialInput = g_simulationData[simulationIndex].z;
        float displacement = displacementInput + disturbance.z;
        float velocityPotential = disturbance.w + velocityPotentialInput;
        g_simulationData[simulationIndex].x = displacement;
        g_simulationData[simulationIndex].z = velocityPotential;
    }
}

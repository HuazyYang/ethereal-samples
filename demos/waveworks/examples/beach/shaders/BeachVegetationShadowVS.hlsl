#define BEACH_SHADOW_VERTEX
#include "BeachLighting.hlsli"
#define main BeachVegetationVertex
#include "BeachVegetationVS.hlsl"
#undef main

float4 main(VSInput input) : SV_Position
{
    // Exactly the visible pass's deformation and simulation clock.
    VSOutput vertex = BeachVegetationVertex(input);
    return mul(g_worldToShadow, float4(vertex.worldPosition, 1));
}

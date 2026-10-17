#define BEACH_SHADOW_VERTEX
#include "BeachLighting.hlsli"
#define main BeachTerrainVertex
#include "BeachTerrainVS.hlsl"
#undef main

float4 main(VSInput input) : SV_Position
{
    return mul(g_worldToShadow, float4(input.position, 1));
}

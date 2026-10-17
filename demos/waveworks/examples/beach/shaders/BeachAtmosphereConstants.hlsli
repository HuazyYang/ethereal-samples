#ifndef WW_REGISTER
#ifdef WAVEWORKS_SPACE
#define WW_REGISTER(reg) register(reg, WAVEWORKS_SPACE)
#else
#define WW_REGISTER(reg) register(reg)
#endif
#endif
#ifndef BEACH_ATMOSPHERE_CONSTANTS_HLSLI
#define BEACH_ATMOSPHERE_CONSTANTS_HLSLI
cbuffer EnvironmentConstants : WW_REGISTER(b0)
{
    float3 g_atmosphereSun; float g_solarIrradiance;
    float g_filterRoughness; float g_viewHeightKm;
    float g_aerosolExtinction; float g_environmentPadding;
    float3 g_viewRight; float g_aerialFarDistance;
    float3 g_viewUp; float g_viewPadding0;
    float3 g_viewForward; float g_viewPadding1;
};
#endif

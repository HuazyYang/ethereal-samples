// Asteroid vertex shader of asteroids_nvrhi. The shader body is the one of the reference sample
// (assets/shaders/asteroid_vs_diligent.vsh); only the resource declarations differ, because nvrhi
// binds by register/space and the binding modes of docs/PLAN.md need different inputs:
//   BINDING_MODE 0 : mut, tex_mut   - one volatile constant buffer per draw (b0)
//   BINDING_MODE 1 : tex_mut_pc     - per-draw data in push constants (b0), view-projection in b1
//   BINDING_MODE 2 : bindless       - per-instance data in a structured buffer (t0), view-projection in b0
#include "shader_common.h"
#include <donut/shaders/binding_helpers.hlsli>

struct AsteroidData
{
    float4x4 World;
    float4   SurfaceColor;

    float DeepColorR;
    float DeepColorG;
    float DeepColorB;
    uint  TextureIndex;
};

struct FrameConstants
{
    float4x4 ViewProjection;
};

#if BINDING_MODE == 0

struct DrawConstants
{
    float4x4     ViewProjection;
    AsteroidData Data;
};
DECLARE_CBUFFER(DrawConstants, g_Draw, 0, 0);

#elif BINDING_MODE == 1

DECLARE_PUSH_CONSTANTS(AsteroidData, g_Push, 0, 0);
DECLARE_CBUFFER(FrameConstants, g_Frame, 1, 0);

#else

DECLARE_CBUFFER(FrameConstants, g_Frame, 0, 0);
StructuredBuffer<AsteroidData> g_Data : REGISTER_SRV(0, 0);

#endif

float linstep(float min, float max, float s)
{
    return saturate((s - min) / (max - min));
}

void main(in float3 in_pos    : POSITION,
          in float3 in_normal : NORMAL,
#if BINDING_MODE == 2
          in uint AsteroidId  : INSTANCEID, // per-instance vertex stream: SV_InstanceID is not affected by the base instance
#endif
          out float4 position : SV_Position,
          out VSOut vs_output)
{
#if BINDING_MODE == 0
    AsteroidData Data           = g_Draw.Data;
    float4x4     ViewProjection = g_Draw.ViewProjection;
#elif BINDING_MODE == 1
    AsteroidData Data           = g_Push;
    float4x4     ViewProjection = g_Frame.ViewProjection;
#else
    AsteroidData Data           = g_Data[AsteroidId];
    float4x4     ViewProjection = g_Frame.ViewProjection;
#endif

    float3 positionWorld = mul(Data.World, float4(in_pos, 1.0f)).xyz;
    position = mul(ViewProjection, float4(positionWorld, 1.0f));

    vs_output.positionModel = in_pos;
    vs_output.normalWorld = mul(Data.World, float4(in_normal, 0.0f)).xyz; // No non-uniform scaling

    float depth = linstep(0.5f, 0.7f, length(in_pos.xyz));
    vs_output.albedo = lerp(float3(Data.DeepColorR, Data.DeepColorG, Data.DeepColorB), Data.SurfaceColor.xyz, depth);

    vs_output.textureId = Data.TextureIndex;
}

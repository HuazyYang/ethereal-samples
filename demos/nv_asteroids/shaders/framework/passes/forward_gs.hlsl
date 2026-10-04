/*
* Asteroids (2018 donut framework) - single-pass-stereo passthrough geometry shader
* (passes/forward_gs.hlsl).
*
* Written for the NVAPI "fast geometry shader" path: it reads only vertex 0 of the triangle,
* emits one vertex with SV_ViewportArrayIndex = 0 and passes NV_X_RIGHT / NV_VIEWPORT_MASK
* through. It only works when the pipeline is created with the NVIDIA fast-GS / single-pass-
* stereo extension (in nvrhi: ShaderDesc::fastGSFlags = ForceFastGS | UseViewportMask and the
* custom semantics NV_X_RIGHT (XRight) and NV_VIEWPORT_MASK (ViewportMask)); with a plain D3D12
* geometry shader a one-vertex triangle strip produces no primitives.
*
* Permutations: MOTION_VECTORS={0,1} (passes PREV_WORLD_POS through, for gbuffer_vs).
*/

#include "../forward_vertex_2018.hlsli"

struct VertexShaderOutput
{
    float4 position : SV_Position;
    SceneVertex vtx;
#if MOTION_VECTORS
    float3 prevWorldPos : PREV_WORLD_POS;
#endif
    float4 positionRight : NV_X_RIGHT;
    uint4 viewportMask : NV_VIEWPORT_MASK;
};

struct GeometryShaderOutput
{
    float4 position : SV_Position;
    SceneVertex vtx;
#if MOTION_VECTORS
    float3 prevWorldPos : PREV_WORLD_POS;
#endif
    uint viewport : SV_ViewportArrayIndex;
    float4 positionRight : NV_X_RIGHT;
    uint4 viewportMask : NV_VIEWPORT_MASK;
};

[maxvertexcount(1)]
void main(
    triangle VertexShaderOutput input[3],
    inout TriangleStream<GeometryShaderOutput> output)
{
    GeometryShaderOutput outputVertex;

    outputVertex.position = input[0].position;
    outputVertex.vtx = input[0].vtx;
#if MOTION_VECTORS
    outputVertex.prevWorldPos = input[0].prevWorldPos;
#endif
    outputVertex.viewport = 0;
    outputVertex.positionRight = input[0].positionRight;
    outputVertex.viewportMask = input[0].viewportMask;

    output.Append(outputVertex);
}

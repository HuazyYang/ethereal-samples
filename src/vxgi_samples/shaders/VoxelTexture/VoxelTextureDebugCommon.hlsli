#ifndef VOXELTEXTUREDEBUGCOMMON_HLSLI
#define VOXELTEXTUREDEBUGCOMMON_HLSLI
#include "../Common/ShaderCommon.hlsli"
cbuffer RaycastCB : register(b1)
{
    float4 g_CameraPos;
    float4 g_GridBounds;
    float4x4 g_ViewProjMatrix;
    float4x4 g_ViewProjMatrixInv;
    int4 g_ToroidalOffset;
    int g_TextureSize;
    uint g_OpacityPackingOffset;
    uint g_EmittancePackingOffset;
    uint g_MipOffset;
    int g_VoxelsToSkip;
    uint g_AllocationMapBit;
    float g_SourceDataScale;
    float g_FarClipZ;
    float g_ViewportMinZ;
    float g_ViewportMaxZ;
    float g_TargetOpacity;
    uint g_SkipUpperLevels;
    uint g_Use6DOpacity;
};
bool IntersectRayBox(float3 rayPos, float3 rcpRayDir, float3 boundaryLow, float3 boundaryHigh, out float tIn, out float tOut, out int directionIn)
{
    float3 t_low = (boundaryLow - rayPos) * rcpRayDir;
    float3 t_high = (boundaryHigh - rayPos) * rcpRayDir;
    float3 t_min = min(t_high, t_low);
    float3 t_max = max(t_high, t_low);
    tIn = max(max(t_min.x, t_min.y), t_min.z);
    tOut = min(min(t_max.x, t_max.y), t_max.z);
    if (tIn == t_min.x) directionIn = 0;
    else if (tIn == t_min.y) directionIn = 1;
    else directionIn = 2;
    return (tOut > tIn) && (tOut > 0);
}
bool OutputFunction(int3 iPos, float3 fracPos, int direction, int3 iStep, out float3 color);
void main(
    VxgiFullScreenQuadOutput IN,
    out float4 gfsdk_ColorOutput: SV_target,
    out float gfsdk_DepthOutput: SV_Depth
)
{
    float3 ray = VxgiProjToRay(IN.posProj, g_CameraPos.xyz, g_ViewProjMatrixInv);
    float3 fPos = (g_CameraPos.xyz - g_GridBounds.xyz) * float3scalar(g_TextureSize / g_GridBounds.w);
    fPos += ray * 0.5;
    gfsdk_ColorOutput = float4(0.0, 0.0, 0.0, 0.0);
    gfsdk_DepthOutput = g_FarClipZ;
    float t = 0;
    int skipLimit = g_VoxelsToSkip;
    float3 rcpRay = float3scalar(1.0) / ray;
    const float margin = pow(2, -8);
    float tIn, tOut;
    int direction;
    if (!IntersectRayBox(fPos, rcpRay, float3scalar(margin), float3scalar(g_TextureSize - margin), tIn, tOut, direction))
{
        discard;
        return;
    }
    tIn = max(tIn, 0);
    tOut -= tIn;
    float3 startPoint = fPos + ray * tIn;
    fPos = startPoint;
    float3 iStep;
    iStep.x = ray.x >= 0 ? 1 : -1;
    iStep.y = ray.y >= 0 ? 1 : -1;
    iStep.z = ray.z >= 0 ? 1 : -1;
    float3 tDelta = abs(rcpRay);
    float3 tMax = (saturate(iStep) - frac(fPos)) * rcpRay;
    t = 0;
    float iBoundsMinUpper = g_TextureSize * 0.25;
    float iBoundsMaxUpper = g_TextureSize * 0.75;

    while (t < tOut)
{
        float3 intersection = startPoint + t * ray;
        fPos = intersection;
        if (direction == 0)
            fPos.x += iStep.x * 0.25;
        else if (direction == 1)
            fPos.y += iStep.y * 0.25;
        else
            fPos.z += iStep.z * 0.25;
        int3 toroidalPos = TOROIDAL_ADDRESS(int3(fPos), g_ToroidalOffset.xyz, g_TextureSize);
        bool skipThisVoxel = false;
        if ((g_SkipUpperLevels > 0) && all(fPos >= float3scalar(iBoundsMinUpper)) && all(fPos < float3scalar(iBoundsMaxUpper)))
{
            skipThisVoxel = true;
        }
        if (!skipThisVoxel && OutputFunction(toroidalPos, frac(intersection), direction, int3(iStep), gfsdk_ColorOutput.rgb))
{
            if (skipLimit == 0)
{
                gfsdk_ColorOutput.a = g_TargetOpacity;
                float3 worldPos = intersection * float3scalar(g_GridBounds.w / g_TextureSize) + g_GridBounds.xyz;
                float4 intersectionProj = mul(float4(worldPos - ray * 0.00001, 1.0), g_ViewProjMatrix);
                intersectionProj.xyz /= intersectionProj.w;

                gfsdk_DepthOutput = intersectionProj.z;

                gfsdk_DepthOutput = gfsdk_DepthOutput * (g_ViewportMaxZ - g_ViewportMinZ) + g_ViewportMinZ;
                return;
            }
            skipLimit -= 1;
        }
        if (tMax.x < min(tMax.y, tMax.z))
{
            t = tMax.x;
            tMax.x += tDelta.x;
            direction = 0;
        }
else if (tMax.y < tMax.z)
{
            t = tMax.y;
            tMax.y += tDelta.y;
            direction = 1;
        }
else
{
            t = tMax.z;
            tMax.z += tDelta.z;
            direction = 2;
        }
    }
    discard;
}
#endif /* VOXELTEXTUREDEBUGCOMMON_HLSLI */

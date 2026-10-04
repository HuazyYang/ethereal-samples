// RenderVolume_VS.hlsl
//
// Light volume vertex shader. Generates the light-volume geometry procedurally from SV_VertexID
// (no vertex/index buffers are bound by the C++ side) in the light's clip space, then transforms it
// to world space (g_mLightToWorld = inverse light view-projection) and to the viewer's clip space.
//
// Callers (ContextImp_D3D11, NvVolumetricLighting.d3d11.dll):
//   MESHMODE_FRUSTUM_GRID  DrawFrustumGrid  (0x18000CC50)  4-cp patches, Draw(4 * res * res)
//   MESHMODE_FRUSTUM_BASE  DrawFrustumBase  (0x18000CDF0)  triangle list, Draw(6)
//   MESHMODE_FRUSTUM_CAP   DrawFrustumCap   (0x18000CFC0)  triangle list, Draw(12 * (res + 1) + 6)
//   MESHMODE_OMNI_VOLUME   DrawOmniVolume   (0x18000D1A0)  4-cp patches, Draw(24 * res * res)
//   MESHMODE_GEOMETRY      (not used by the C++; vertices come from a POSITION stream)
// Table: vs_RenderVolume @ 0x1801FF380 (8 entries, index = MESHMODE)
//
// res = g_uMeshResolution (VolumeDesc::uMaxMeshResolution / tessellation factor).

#include "ShaderCommon.hlsli"
#include "RenderVolumeGeom.hlsli"

#define MESHMODE_FRUSTUM_GRID 0
#define MESHMODE_FRUSTUM_BASE 1
#define MESHMODE_FRUSTUM_CAP 2
#define MESHMODE_OMNI_VOLUME 3
#define MESHMODE_GEOMETRY 4

#ifndef MESHMODE
#define MESHMODE MESHMODE_FRUSTUM_GRID
#endif

// Corner of a grid quad patch: 0 -> (0,0), 1 -> (1,0), 2 -> (1,1), 3 -> (0,1)
float2 GetQuadCorner(uint corner)
{
    return (corner == 0) ? float2(0, 0) : (corner == 1) ? float2(1, 0) : (corner == 2) ? float2(1, 1) : float2(0, 1);
}

// Position of a grid patch corner in [-1,1]^2 for a g_uMeshResolution x g_uMeshResolution grid
float2 GetGridPosition(uint patchId, float2 vCorner)
{
    float fStep = 2.0f / float(g_uMeshResolution);
    float2 vGrid;
    vGrid.x = fStep * float(patchId % g_uMeshResolution) - 1.0f;
    vGrid.y = fStep * float(patchId / g_uMeshResolution) - 1.0f;
    return vGrid + fStep * vCorner;
}

#if (MESHMODE == MESHMODE_GEOMETRY)
VS_POLYGONAL_OUTPUT main(float4 vPos : POSITION, uint id : SV_VERTEXID)
#else
VS_POLYGONAL_OUTPUT main(uint id : SV_VERTEXID)
#endif
{
    VS_POLYGONAL_OUTPUT output;
    float4 vClipPos;
    float4 vWorldPos;

#if (MESHMODE == MESHMODE_FRUSTUM_GRID)
    // Regular res x res grid of quad patches covering the light frustum's far plane.
    float2 vCorner = GetQuadCorner(id & 3);
    vClipPos = float4(GetGridPosition(id >> 2, vCorner), 1, 1);
    vWorldPos = mul(g_mLightToWorld, vClipPos);
#elif (MESHMODE == MESHMODE_FRUSTUM_BASE)
    // Two triangles covering the far plane of the frustum.
    uint triId = id / 3;
    uint vertId = id % 3;
    float2 vVert;
    vVert.y = (vertId == 2) ? -1.0f : 1.0f;
    vVert.x = (vertId == 0) ? 1.0f : -1.0f;
    vClipPos = float4(vVert * ((triId == 0) ? 1.0f : -1.0f), 1, 1);
    vWorldPos = mul(g_mLightToWorld, vClipPos);
#elif (MESHMODE == MESHMODE_FRUSTUM_CAP)
    // Side faces of the frustum (from the apex at z = 0 to the far plane edge), tessellated along
    // the far edge to match the grid, followed by the near cap (two triangles at z = 0).
    uint vertsPerFace = 3 * (g_uMeshResolution + 1);
    uint faceId = id / vertsPerFace;
    uint vertId = id % 3;
    float3 vPos;
    if (faceId < 4)
    {
        float fStep = 2.0f / float(g_uMeshResolution);
        uint halfRes = (1 + g_uMeshResolution) >> 1;
        uint triId = (id % vertsPerFace) / 3;
        float fEdge;
        float fDepth;
        if (triId < g_uMeshResolution)
        {
            fEdge = (vertId == 0) ? ((triId >= halfRes) ? 1.0f : -1.0f) : ((vertId == 1) ? fStep * float(triId) - 1.0f : fStep * float(triId + 1) - 1.0f);
            fDepth = (vertId == 0) ? 0.0f : 1.0f;
        }
        else
        {
            fEdge = (vertId == 1) ? fStep * float(halfRes) - 1.0f : ((vertId == 0) ? -1.0f : 1.0f);
            fDepth = (vertId == 1) ? 1.0f : 0.0f;
        }
        float fSign = (faceId >> 1) ? -1.0f : 1.0f;
        float2 vEdge = float2(fEdge, 1) * fSign;
        vPos = (faceId & 1) ? float3(vEdge.x, fSign, fDepth) * float3(-1, 1, 1) : float3(vEdge.y, vEdge.x, fDepth);
    }
    else
    {
        uint triId = (id - 4 * vertsPerFace) / 3;
        vPos = float3(float2((vertId == 1) ? 1.0f : -1.0f, (vertId == 2) ? 1.0f : -1.0f) * ((triId == 0) ? 1.0f : -1.0f), 0);
    }
    vClipPos = float4(vPos, 1);
    vWorldPos = mul(g_mLightToWorld, vClipPos);
#elif (MESHMODE == MESHMODE_OMNI_VOLUME)
    // Cube of 6 faces, each a res x res grid of quad patches; the vertices are projected onto the
    // sphere of radius g_fLightZFar around the light (paraboloid/omni volume).
    float2 vCorner = GetQuadCorner(id & 3);
    uint patchesPerFace = 4 * g_uMeshResolution * g_uMeshResolution;
    uint faceId = id / patchesPerFace;
    uint patchId = (id % patchesPerFace) >> 2;
    uint faceAxis = faceId % 3;
    float2 vUV = GetGridPosition(patchId, vCorner);
    // Face axis = faceId % 3, face sign = (faceId / 3) ? -1 : 1. Faces: +-X = (s, s*u, v),
    // +-Y = (-s*u, s, v), +-Z = (s*u, v, s). The sign vectors are written as in the original
    // dataflow (vector selects multiplied in), which fxc keeps unfolded.
    float3 vSignY = ((faceId / 3) == 1) ? float3(1, 1, 1) : float3(-1, 1, 1);
    float2 vFaceSign = (faceId / 3) ? float2(-1, 1) : float2(1, 1);
    float3 vDir;
    if (faceAxis == 0)
        vDir = vFaceSign.yxy * float3(vFaceSign.x, vUV.x, vUV.y);
    else if (faceAxis == 1)
        vDir = float3(vUV.x, vFaceSign.x, vUV.y) * vSignY;
    else
        vDir = (vFaceSign.yxy * float3(vFaceSign.x, vUV.x, vUV.y)).yzx;
    vDir = normalize(vDir);
    vClipPos = float4(vDir, 1);
    vWorldPos = mul(g_mLightToWorld, float4(vDir * g_fLightZFar, 1));
#else // MESHMODE_GEOMETRY
    vClipPos = vPos;
    vWorldPos = mul(g_mLightToWorld, vClipPos);
#endif

    vWorldPos = vWorldPos / vWorldPos.w;
    output.vPos = mul(g_mViewProj, vWorldPos);
    output.vWorldPos = vWorldPos;
    output.vClipPos = vClipPos;
    return output;
}

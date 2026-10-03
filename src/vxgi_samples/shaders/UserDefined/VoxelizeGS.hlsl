#include "VoxelizationCommon.hlsli"
Texture3D<uint> t_InvalidateBitmap : REGISTER_SRV(VXGI_INVALIDATE_BITMAP_SRV_SLOT, VXGI_RESOURCE_SPACE);
struct UserAttributesWithPos {
    float3 positionWS : POS;
    float2 texCoord : TEXCOORD;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float4 position : SV_Position;
};
struct Voxelize_GSIn {
    UserAttributesWithPos UserData;
};
struct Voxelize_GSOut {
    UserAttributesWithPos UserData;
    VxgiVoxelizationGSOutputData VXGIData;
};
bool IsTriangleInReadOnlyPage(float3 lower, float3 upper)
{
    float3 lowerPage = (lower * 0.5 + 0.5) * float(g_VxgiVoxelizationCB.AllocationMapSize);
    float3 upperPage = (upper * 0.5 + 0.5) * float(g_VxgiVoxelizationCB.AllocationMapSize);
    uint mask = ~0u;
    int3 page = int3(floor(lowerPage));
    if (all(floor(lowerPage) == floor(upperPage))) {
        mask = t_InvalidateBitmap[int3(page.xy, page.z >> 5)].x;
    }
    return ((mask & (1u << (page.z & 31))) == 0);
}
#if EXPLICIT_FAST_GS
int IntersectTriangleWithScissorBoxes(float3 lower, float3 upper)
{
#if TEST_SCISSOR_REGIONS_IN_GS
    int result = 0;
    float finerLevelSize = g_VxgiVoxelizationCB.DiscardClipSpace;
    for (int level = 0; level < 5; level++) {
        finerLevelSize *= 2;
        VxgiBox4f levelScissor = g_VxgiVoxelizationCB.ScissorRegionsClipSpace[level];
        if (any(lower > levelScissor.upper.xyz) || any(upper < levelScissor.lower.xyz))
            continue;
        if ((level >= g_VxgiVoxelizationCB.FirstLevelToDiscard) && all(lower.xyz >= float3scalar(-finerLevelSize)) && all(upper <= float3scalar(finerLevelSize)))
            continue;
        result |= (1u << (level * 3));
    }
    return result;
#else
    return ~0;
#endif
}
#else
bool TestTriangleDiscard(float3 lower, float3 upper, int level)
{
#if TEST_SCISSOR_REGIONS_IN_GS
    float finerLevelSize = g_VxgiVoxelizationCB.DiscardClipSpace * float(1u << level);
    VxgiBox4f levelScissor = g_VxgiVoxelizationCB.ScissorRegionsClipSpace[level];
    if (any(lower > levelScissor.upper.xyz) || any(upper < levelScissor.lower.xyz))
        return true;
    if ((level >= g_VxgiVoxelizationCB.FirstLevelToDiscard) && all(lower.xyz >= float3scalar(-finerLevelSize)) && all(upper.xyz <= float3scalar(finerLevelSize)))
        return true;
#endif
    return false;
}
#endif
#if COVERAGE_WITH_EDGE_EQUATIONS
float3 GetEdgeEquation(float2 v1, float2 v2)
{
    float a = v1.y - v2.y;
    float b = v2.x - v1.x;
    float c = -0.5 * (a * (v1.x + v2.x) + b * (v1.y + v2.y));
    return float3(a, b, c);
}
#endif
#if USE_SOFTWARE_CONSERVATIVE_RASTER
float2 Intersect2D(float2 p00, float2 p01, float2 p10, float2 p11)
{
    float2 _point;
    float d = rcp((p00.x - p01.x) * (p10.y - p11.y) - (p00.y - p01.y) * (p10.x - p11.x));
    _point.x = ((p10.x - p11.x) * (p00.x * p01.y - p00.y * p01.x) - (p00.x - p01.x) * (p10.x * p11.y - p10.y * p11.x)) * d;
    _point.y = ((p10.y - p11.y) * (p00.x * p01.y - p00.y * p01.x) - (p00.y - p01.y) * (p10.x * p11.y - p10.y * p11.x)) * d;
    return _point;
}
void RasterizeConservative2D(inout float2 p0, inout float2 p1, inout float2 p2, float2 rOffset)
{
    float2 edge0 = p0 - p2;
    float2 edge1 = p1 - p0;
    float2 edge2 = p2 - p1;
    float frontFace = edge0.x * edge1.y - edge0.y * edge1.x;
    float2 normal0 = float2(edge0.y * frontFace, -edge0.x * frontFace);
    float2 normal1 = float2(edge1.y * frontFace, -edge1.x * frontFace);
    float2 normal2 = float2(edge2.y * frontFace, -edge2.x * frontFace);
    float2 offsetDir0 = float2(sign(normal0.x) * rOffset.x, sign(normal0.y) * rOffset.y);
    float2 offsetDir1 = float2(sign(normal1.x) * rOffset.x, sign(normal1.y) * rOffset.y);
    float2 offsetDir2 = float2(sign(normal2.x) * rOffset.x, sign(normal2.y) * rOffset.y);
    float2 _p0 = Intersect2D(p0 + offsetDir0, p2 + offsetDir0, p0 + offsetDir1, p1 + offsetDir1);
    float2 _p1 = Intersect2D(p1 + offsetDir1, p0 + offsetDir1, p1 + offsetDir2, p2 + offsetDir2);
    float2 _p2 = Intersect2D(p2 + offsetDir2, p1 + offsetDir2, p2 + offsetDir0, p0 + offsetDir0);
    p0 = _p0;
    p1 = _p1;
    p2 = _p2;
}
void ExtrapolateAttribute(inout float z1, inout float z2, inout float z3, float3x3 factors, float2 newp1, float2 newp2, float2 newp3)
{
    float3 equation = mul(transpose(factors), float3(z1, z2, z3));
    z1 = dot(equation.xy, newp1) + equation.z;
    z2 = dot(equation.xy, newp2) + equation.z;
    z3 = dot(equation.xy, newp3) + equation.z;
}
void ExtrapolateAttribute(inout float2 a1, inout float2 a2, inout float2 a3,
                          float3x3 factors, float2 newp1, float2 newp2,
                          float2 newp3)
{
    float3x2 equation = mul(transpose(factors), float3x2(a1, a2, a3));
    a1 = mul(float3(newp1, 1.0), equation);
    a2 = mul(float3(newp2, 1.0), equation);
    a3 = mul(float3(newp3, 1.0), equation);
}
void ExtrapolateAttribute(inout float3 a1, inout float3 a2, inout float3 a3,
                          float3x3 factors, float2 newp1, float2 newp2,
                          float2 newp3)
{
    float3x3 equation = mul(transpose(factors), float3x3(a1, a2, a3));
    a1 = mul(float3(newp1, 1.0), equation);
    a2 = mul(float3(newp2, 1.0), equation);
    a3 = mul(float3(newp3, 1.0), equation);
}
void ExtrapolateTangentAttribute(inout float4 a1, inout float4 a2, inout float4 a3,
                          float3x3 factors, float2 newp1, float2 newp2,
                          float2 newp3)
{
    float3x3 equation = mul(transpose(factors), float3x3(a1.xyz, a2.xyz, a3.xyz));
    a1.xyz = mul(float3(newp1, 1.0), equation);
    a2.xyz = mul(float3(newp2, 1.0), equation);
    a3.xyz = mul(float3(newp3, 1.0), equation);
}
#define EXTRAPOLATE_USER_ATTRS                                                                                                                    \
    ExtrapolateAttribute(TMP[0].UserData.positionWS, TMP[1].UserData.positionWS, TMP[2].UserData.positionWS, factors, newPos0, newPos1, newPos2); \
    ExtrapolateAttribute(TMP[0].UserData.texCoord, TMP[1].UserData.texCoord, TMP[2].UserData.texCoord, factors, newPos0, newPos1, newPos2);       \
    ExtrapolateAttribute(TMP[0].UserData.normal, TMP[1].UserData.normal, TMP[2].UserData.normal, factors, newPos0, newPos1, newPos2);             \
    ExtrapolateTangentAttribute(TMP[0].UserData.tangent, TMP[1].UserData.tangent, TMP[2].UserData.tangent, factors, newPos0, newPos1, newPos2);          \
    /* ExtrapolateAttribute(TMP[0].UserData.binormal, TMP[1].UserData.binormal, TMP[2].UserData.binormal, factors, newPos0, newPos1, newPos2); */
#endif
#define SV_POSITION_ATTR_IN(n) IN[n].UserData.position
bool CullTriangle(float3 v1, float3 v2, float3 v3, float3 normal) { return false; }
#if EXPLICIT_FAST_GS
[maxvertexcount(1)]
#else
[instance(CLIP_LEVELS_TO_INSTANCE)]
[maxvertexcount(3)]
#endif
void main(triangle Voxelize_GSIn IN[3], int gfsdk_InvocationID: SV_GSInstanceID, inout TriangleStream<Voxelize_GSOut> outStream)
{
    Voxelize_GSOut OUT;
#if !EXPLICIT_FAST_GS
    int clipLevel = gfsdk_InvocationID;
    if ((g_VxgiVoxelizationCB.ClipLevelMask & (1u << (clipLevel * 3))) != 0)
#endif
    {
        float3 pos0 = SV_POSITION_ATTR_IN(0).xyz;
        float3 pos1 = SV_POSITION_ATTR_IN(1).xyz;
        float3 pos2 = SV_POSITION_ATTR_IN(2).xyz;
        float3 edge01 = pos1.xyz - pos0.xyz;
        float3 edge02 = pos2.xyz - pos0.xyz;
        float3 normal = normalize(cross(edge01, edge02));
        bool culled = false;
        if ((g_VxgiVoxelizationCB.UseCullFunction != 0) && CullTriangle(pos0, pos1, pos2, normal))
            culled = true;
        float3 lower = min(min(pos0, pos1), pos2);
        float3 upper = max(max(pos0, pos1), pos2);
        if (bool(g_VxgiVoxelizationCB.UseInvalidateBitmap)) {
            if (IsTriangleInReadOnlyPage(lower, upper))
                culled = true;
        }
#if EXPLICIT_FAST_GS
        int clipLevelMask = int(g_VxgiVoxelizationCB.ClipLevelMask) & IntersectTriangleWithScissorBoxes(lower, upper);
        if (culled)
            clipLevelMask = 0;
#else
        if (TestTriangleDiscard(lower, upper, clipLevel))
            culled = true;
        if (culled)
            return;
#endif
        float3 absNormal = abs(normal);
        int projectionIndex = 0;
        if (absNormal.z > absNormal.y && absNormal.z > absNormal.x) {
            pos0 = pos0.xyz;
            pos1 = pos1.xyz;
            pos2 = pos2.xyz;
            projectionIndex = 0;
        } else if (absNormal.y > absNormal.x) {
            pos0 = pos0.zxy;
            pos1 = pos1.zxy;
            pos2 = pos2.zxy;
            projectionIndex = 1;
        } else {
            pos0 = pos0.yzx;
            pos1 = pos1.yzx;
            pos2 = pos2.yzx;
            projectionIndex = 2;
        }
        Voxelize_GSIn TMP[3];
        TMP[0] = IN[0];
        TMP[1] = IN[1];
        TMP[2] = IN[2];
#if COVERAGE_WITH_EDGE_EQUATIONS
        float doubleArea = (-pos1.x * pos0.y + pos2.x * pos0.y + pos0.x * pos1.y - pos2.x * pos1.y - pos0.x * pos2.y + pos1.x * pos2.y);
        float halfInvArea = rcp(doubleArea);
        float areaSign = sign(doubleArea);
        halfInvArea = abs(halfInvArea);
        OUT.VXGIData.Edge1.xyz = GetEdgeEquation(pos0.xy, pos1.xy) * areaSign;
        OUT.VXGIData.Edge2.xyz = GetEdgeEquation(pos1.xy, pos2.xy) * areaSign;
        OUT.VXGIData.Edge3.xyz = GetEdgeEquation(pos2.xy, pos0.xy) * areaSign;
        OUT.VXGIData.ProjectedBoundingBox.xy = min(min(pos0.xy, pos1.xy), pos2.xy);
        OUT.VXGIData.ProjectedBoundingBox.zw = max(max(pos0.xy, pos1.xy), pos2.xy);
// Software conservative rasterization needs 'clipLevel', which only exists in the non-explicit-fast-GS
// path. That combination is never requested; the guard keeps it compilable as part of Shaders.cfg's
// brace product.
#if USE_SOFTWARE_CONSERVATIVE_RASTER && !EXPLICIT_FAST_GS
        float2 newPos0 = pos0.xy;
        float2 newPos1 = pos1.xy;
        float2 newPos2 = pos2.xy;
        float halfVoxelSize = rcp(g_VxgiVoxelizationCB.ClipLevelSize) * pow(0.5, g_VxgiVoxelizationCB.MaxClipLevel - clipLevel - 1) * g_VxgiVoxelizationMaterialCB.ResolutionFactors[clipLevel].y;
        RasterizeConservative2D(newPos0, newPos1, newPos2, float2(halfVoxelSize, halfVoxelSize));
        float3x3 factors;
        factors[0] = OUT.VXGIData.Edge2.xyz * halfInvArea;
        factors[1] = OUT.VXGIData.Edge3.xyz * halfInvArea;
        factors[2] = OUT.VXGIData.Edge1.xyz * halfInvArea;
        ExtrapolateAttribute(pos0.z, pos1.z, pos2.z, factors, newPos0, newPos1, newPos2);
        pos0.xy = newPos0;
        pos1.xy = newPos1;
        pos2.xy = newPos2;
        EXTRAPOLATE_USER_ATTRS
#endif
#endif
#if EXPLICIT_FAST_GS
        OUT.VXGIData.gl_ViewportIndex = clipLevelMask << projectionIndex;
        OUT.UserData = TMP[0].UserData;
        outStream.Append(OUT);
        outStream.RestartStrip();
#else
        OUT.VXGIData.gl_ViewportIndex = clipLevel * 3 + projectionIndex;
        OUT.UserData = TMP[0].UserData;
        OUT.UserData.position = float4(pos0.xyz, 1);
        outStream.Append(OUT);
        OUT.UserData = TMP[1].UserData;
        OUT.UserData.position = float4(pos1.xyz, 1);
        outStream.Append(OUT);
        OUT.UserData = TMP[2].UserData;
        OUT.UserData.position = float4(pos2.xyz, 1);
        outStream.Append(OUT);
        outStream.RestartStrip();
#endif
    }
}

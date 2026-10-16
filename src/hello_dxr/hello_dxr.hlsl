//
// Hello DXR: primary rays plus one shadow ray per hit.
//
// Ported from D3D12-Trial/HelloDXR (HelloDXR_Simple.hlsl). The original bound the per-instance
// vertex/index buffers and normal matrices through local root signatures, one shader record per
// instance. Here every resource is bound once through a single global binding layout and the hit
// shader looks its instance up through InstanceIndex(), so the same library runs on D3D12 and Vulkan
// (nvrhi has no local bindings on Vulkan).
//

#pragma pack_matrix(row_major)

struct FrameConstants
{
    float4x4 clipToWorld;
    float4   eyePosition;   // xyz
    float4   lightPosition; // xyz, the point light orbiting the scene
    float4   lightStrength; // rgb
};

struct InstanceData
{
    float4x4 normalMatrix;  // upper-left 3x3: local-to-world inverse transpose
    uint     vertexOffset;  // first vertex of the mesh in g_Vertices
    uint     indexOffset;   // first index of the mesh in g_Indices
    uint2    padding;
};

// 16-byte members only, so that the structured buffer has the same layout as the C++ side on D3D12 and Vulkan.
struct Vertex
{
    float4 position; // xyz
    float4 normal;   // xyz
    float4 color;
};

struct HitInfo
{
    float4 colorAndDistance;
};

struct ShadowHitInfo
{
    uint isHit; // 1 while occluded; ShadowMiss clears it
};

struct Attributes
{
    float2 bary;
};

ConstantBuffer<FrameConstants>  g_Frame     : register(b0);
RaytracingAccelerationStructure g_SceneBVH  : register(t0);
StructuredBuffer<InstanceData>  g_Instances : register(t1);
StructuredBuffer<Vertex>        g_Vertices  : register(t2);
StructuredBuffer<uint>          g_Indices   : register(t3);
RWTexture2D<float4>             u_Output    : register(u0);

[shader("raygeneration")]
void RayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    float2 dims = float2(DispatchRaysDimensions().xy);
    float2 d = ((float2(launchIndex) + 0.5f) / dims) * 2.0f - 1.0f;
    d.y = -d.y;

    // Unproject the near and far points of the pixel; the ray starts at the eye.
    float4 pNear = mul(float4(d, 0.0f, 1.0f), g_Frame.clipToWorld);
    float4 pFar  = mul(float4(d, 1.0f, 1.0f), g_Frame.clipToWorld);
    pNear /= pNear.w;
    pFar  /= pFar.w;

    RayDesc ray;
    ray.Origin    = g_Frame.eyePosition.xyz;
    ray.Direction = normalize(pFar.xyz - pNear.xyz);
    ray.TMin      = 0.0f;
    ray.TMax      = 100000.0f;

    HitInfo payload;
    payload.colorAndDistance = float4(0, 0, 0, 0);

    // Hit group 0 (HitGroup), miss shader 0 (Miss).
    TraceRay(g_SceneBVH, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

    u_Output[launchIndex] = float4(payload.colorAndDistance.rgb, 1.0f);
}

[shader("miss")]
void Miss(inout HitInfo payload : SV_RayPayload)
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    float2 dims = float2(DispatchRaysDimensions().xy);
    payload.colorAndDistance = float4(0.2f, 0.2f, -0.6f * launchIndex.y / dims.y + 1.0f, -1.0f);
}

[shader("closesthit")]
void ClosestHit(inout HitInfo payload, Attributes attrib)
{
    float3 posW = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
    float3 lightDir = normalize(g_Frame.lightPosition.xyz - posW);

    // Shadow ray: hit group 1 (ShadowHitGroup, no closest-hit shader), miss shader 1 (ShadowMiss).
    RayDesc ray;
    ray.Origin    = posW;
    ray.Direction = lightDir;
    ray.TMin      = 0.01f;
    ray.TMax      = 100000.0f;

    ShadowHitInfo shadowPayload;
    shadowPayload.isHit = 1;
    TraceRay(g_SceneBVH, RAY_FLAG_NONE, 0xFF, 1, 0, 1, ray, shadowPayload);

    float shadowFactor = shadowPayload.isHit ? 0.3f : 1.0f;

    // InstanceIndex(): the position in the TLAS instance array, which is also the index into g_Instances.
    InstanceData inst = g_Instances[InstanceIndex()];
    uint vid = inst.indexOffset + 3 * PrimitiveIndex();
    Vertex v0 = g_Vertices[inst.vertexOffset + g_Indices[vid + 0]];
    Vertex v1 = g_Vertices[inst.vertexOffset + g_Indices[vid + 1]];
    Vertex v2 = g_Vertices[inst.vertexOffset + g_Indices[vid + 2]];

    float3 barycentrics = float3(1.0f - attrib.bary.x - attrib.bary.y, attrib.bary.x, attrib.bary.y);
    float4 color   = v0.color * barycentrics.x + v1.color * barycentrics.y + v2.color * barycentrics.z;
    float3 normalL = v0.normal.xyz * barycentrics.x + v1.normal.xyz * barycentrics.y + v2.normal.xyz * barycentrics.z;
    float3 normalW = normalize(mul(normalL, (float3x3)inst.normalMatrix));

    payload.colorAndDistance = color * g_Frame.lightStrength * shadowFactor * max(dot(normalW, lightDir), 0.0f);
}

[shader("miss")]
void ShadowMiss(inout ShadowHitInfo payload : SV_RayPayload)
{
    payload.isHit = 0;
}

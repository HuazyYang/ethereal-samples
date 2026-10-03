/*
* Copyright (c) 2019-2023, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "Descriptors.hlsli"
#include "RayTracing.hlsli"

[shader("closesthit")]
void CHS_LOD0(inout PackedPayload packedPayload, BuiltInTriangleIntersectionAttributes attrib)
{
    Payload payload = (Payload)0;
    payload.hitT = RayTCurrent();
    payload.hitKind = HitKind();

    // Load the intersected mesh geometry's data
    GeometryData geometry;
    GetGeometryData(InstanceID(), GeometryIndex(), geometry);

    // Load the triangle's vertices
    Vertex vertices[3];
    LoadVertices(PrimitiveIndex(), geometry, vertices);

    // Interpolate the triangle's attributes for the hit location (position, normal, tangent, texture coordinates)
    float3 barycentrics = float3((1.f - attrib.barycentrics.x - attrib.barycentrics.y), attrib.barycentrics.x, attrib.barycentrics.y);
    Vertex v = InterpolateVertex(vertices, barycentrics);

    // World position
    payload.worldPosition = v.position;
    payload.worldPosition = mul(float4(payload.worldPosition, 1.f), ObjectToWorld4x3()).xyz; // instance transform

    // Geometric normal
    payload.normal = v.normal;
    payload.normal = normalize(mul(float4(payload.normal, 0.f), ObjectToWorld4x3()).xyz);
    payload.shadingNormal = payload.normal;

    // Load the surface material
    MaterialConstants material = GetMaterial(geometry);
    payload.albedo = material.baseOrDiffuseColor;
    payload.opacity = material.opacity;

    // Albedo and Opacity
    if (material.baseOrDiffuseTextureIndex > -1)
    {
        float4 bco = GetSceneTex2D(material.baseOrDiffuseTextureIndex).SampleLevel(GetBilinearWrapSampler(), v.uv0, 0);
        payload.albedo *= bco.rgb;
        payload.opacity *= bco.a;
    }

    // Shading normal
    if (material.normalTextureIndex > -1)
    {
        float3 tangent = normalize(mul(float4(v.tangent.xyz, 0.f), ObjectToWorld4x3()).xyz);
        float3 bitangent = cross(payload.normal, tangent) * v.tangent.w;
        float3x3 TBN = { tangent, bitangent, payload.normal };
        payload.shadingNormal = GetSceneTex2D(material.normalTextureIndex).SampleLevel(GetBilinearWrapSampler(), v.uv0, 0).xyz;
        payload.shadingNormal = (payload.shadingNormal * 2.f) - 1.f;    // Transform to [-1, 1]
        payload.shadingNormal = mul(payload.shadingNormal, TBN);        // Transform tangent-space normal to world-space
    }

    // Roughness and Metallic
    if (material.metalRoughOrSpecularTextureIndex > -1)
    {
        float2 rm = GetSceneTex2D(material.metalRoughOrSpecularTextureIndex).SampleLevel(GetBilinearWrapSampler(), v.uv0, 0).gb;
        payload.roughness = rm.x;
        payload.metallic = rm.y;
    }

    // Emissive
    if (material.emissiveTextureIndex > -1)
    {
        payload.albedo += GetSceneTex2D(material.emissiveTextureIndex).SampleLevel(GetBilinearWrapSampler(), v.uv0, 0).rgb;
    }

    // Pack the payload
    packedPayload = PackPayload(payload);
}

[shader("closesthit")]
void CHS_PRIMARY(inout PackedPayload packedPayload, BuiltInTriangleIntersectionAttributes attrib)
{
    Payload payload = (Payload)0;
    payload.hitT = RayTCurrent();
    payload.hitKind = HitKind();

    // Load the intersected mesh geometry's data
    GeometryData geometry;
    GetGeometryData(InstanceID(), GeometryIndex(), geometry);

    // Load the triangle's vertices
    Vertex vertices[3];
    LoadVertices(PrimitiveIndex(), geometry, vertices);

    // Interpolate the triangle's attributes for the hit location (position, normal, tangent, texture coordinates)
    float3 barycentrics = float3((1.f - attrib.barycentrics.x - attrib.barycentrics.y), attrib.barycentrics.x, attrib.barycentrics.y);
    Vertex v = InterpolateVertex(vertices, barycentrics);

    // World position
    payload.worldPosition = v.position;
    payload.worldPosition = mul(float4(payload.worldPosition, 1.f), ObjectToWorld4x3()).xyz; // instance transform

    // Geometric normal
    payload.normal = v.normal;
    payload.normal = normalize(mul(float4(payload.normal, 0.f), ObjectToWorld4x3()).xyz);
    payload.shadingNormal = payload.normal;

    // Load the surface material
    MaterialConstants material = GetMaterial(geometry);
    payload.albedo = material.baseOrDiffuseColor;
    payload.opacity = material.opacity;

    // Compute texture coordinate differentials
    float2 dUVdx, dUVdy;
    ComputeUV0Differentials(vertices, WorldRayDirection(), RayTCurrent(), dUVdx, dUVdy);

    // TODO-ACM: passing ConstantBuffer<T> to functions crashes DXC HLSL->SPIRV
    //ConstantBuffer<Camera> camera = GetCamera();
    //ComputeUV0Differentials(vertices, camera, WorldRayDirection(), RayTCurrent(), dUVdx, dUVdy);

    // Albedo and Opacity
    if (material.baseOrDiffuseTextureIndex > -1)
    {
        float4 bco = GetSceneTex2D(material.baseOrDiffuseTextureIndex).SampleGrad(GetAnisoWrapSampler(), v.uv0, dUVdx, dUVdy);
        payload.albedo *= bco.rgb;
        payload.opacity *= bco.a;
    }

    // Shading normal
    if (material.normalTextureIndex > -1)
    {
        float3 tangent = normalize(mul(float4(v.tangent.xyz, 0.f), ObjectToWorld4x3()).xyz);
        float3 bitangent = cross(payload.normal, tangent) * v.tangent.w;
        float3x3 TBN = { tangent, bitangent, payload.normal };

        payload.shadingNormal = GetSceneTex2D(material.normalTextureIndex).SampleGrad(GetAnisoWrapSampler(), v.uv0, dUVdx, dUVdy).xyz;
        payload.shadingNormal = (payload.shadingNormal * 2.f) - 1.f;    // Transform to [-1, 1]
        payload.shadingNormal = mul(payload.shadingNormal, TBN);        // Transform tangent-space normal to world-space
    }

    // Roughness and Metallic
    if (material.metalRoughOrSpecularTextureIndex > -1)
    {
        float2 rm = GetSceneTex2D(material.metalRoughOrSpecularTextureIndex).SampleGrad(GetAnisoWrapSampler(), v.uv0, dUVdx, dUVdy).gb;
        payload.roughness = rm.x;
        payload.metallic = rm.y;
    }

    // Emissive
    if (material.emissiveTextureIndex > -1)
    {
        payload.albedo += GetSceneTex2D(material.emissiveTextureIndex).SampleGrad(GetAnisoWrapSampler(), v.uv0, dUVdx, dUVdy).rgb;
    }

    // Pack the payload
    packedPayload = PackPayload(payload);
}

[shader("closesthit")]
void CHS_GI(inout PackedPayload packedPayload, BuiltInTriangleIntersectionAttributes attrib)
{
    Payload payload = (Payload)0;
    payload.hitT = RayTCurrent();
    payload.hitKind = HitKind();

    // Load the intersected mesh geometry's data
    GeometryData geometry;
    GetGeometryData(InstanceID(), GeometryIndex(), geometry);

    // Load the triangle's vertices
    Vertex vertices[3];
    LoadVertices(PrimitiveIndex(), geometry, vertices);

    // Interpolate the triangle's attributes for the hit location (position, normal, tangent, texture coordinates)
    float3 barycentrics = float3((1.f - attrib.barycentrics.x - attrib.barycentrics.y), attrib.barycentrics.x, attrib.barycentrics.y);
    Vertex v = InterpolateVertex(vertices, barycentrics);

    // World position
    payload.worldPosition = v.position;
    payload.worldPosition = mul(float4(payload.worldPosition, 1.f), ObjectToWorld4x3()).xyz; // instance transform

    // Geometric normal
    payload.normal = v.normal;
    payload.normal = normalize(mul(float4(payload.normal, 0.f), ObjectToWorld4x3()).xyz);
    payload.shadingNormal = payload.normal;

    // Load the surface material
    MaterialConstants material = GetMaterial(geometry);
    payload.albedo = material.baseOrDiffuseColor;
    payload.opacity = material.opacity;

    // Albedo and Opacity
    if (material.baseOrDiffuseTextureIndex > -1)
    {
        // Get the number of mip levels
        uint width, height, numLevels;
        GetSceneTex2D(material.baseOrDiffuseTextureIndex).GetDimensions(0, width, height, numLevels);

        // Sample the albedo texture
        float4 bco = GetSceneTex2D(material.baseOrDiffuseTextureIndex).SampleLevel(GetBilinearWrapSampler(), v.uv0, numLevels / 2.f);
        payload.albedo *= bco.rgb;
        payload.opacity *= bco.a;
    }

    // Shading normal
    if (material.normalTextureIndex > -1)
    {
        // Get the number of mip levels
        uint width, height, numLevels;
        GetSceneTex2D(material.normalTextureIndex).GetDimensions(0, width, height, numLevels);

        float3 tangent = normalize(mul(float4(v.tangent.xyz, 0.f), ObjectToWorld4x3()).xyz);
        float3 bitangent = cross(payload.normal, tangent) * v.tangent.w;
        float3x3 TBN = { tangent, bitangent, payload.normal };
        payload.shadingNormal = GetSceneTex2D(material.normalTextureIndex).SampleLevel(GetBilinearWrapSampler(), v.uv0, numLevels / 2.f).xyz;
        payload.shadingNormal = (payload.shadingNormal * 2.f) - 1.f;    // Transform to [-1, 1]
        payload.shadingNormal = mul(payload.shadingNormal, TBN);        // Transform tangent-space normal to world-space
    }

    // Pack the payload
    packedPayload = PackPayload(payload);
}

[shader("closesthit")]
void CHS_VISIBILITY(inout PackedPayload packedPayload, BuiltInTriangleIntersectionAttributes attrib)
{
    packedPayload.hitT = RayTCurrent();
}


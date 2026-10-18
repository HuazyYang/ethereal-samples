// Same scene and local binding contract on DXR, Vulkan RT, and OptiX.
#ifndef TARGET_VULKAN
#define TARGET_VULKAN 0
#endif
struct Material { uint value; uint3 padding; };
struct RaySettings { float firstX; float stepX; uint2 padding; };
struct Payload { uint value; };
#if TARGET_VULKAN
typedef Material* LocalMaterial;
typedef RaySettings* LocalSettings;
#else
typedef ConstantBuffer<Material> LocalMaterial;
typedef ConstantBuffer<RaySettings> LocalSettings;
#endif
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint> output : register(u0);
[shader("raygeneration")]
void rayGeneration(uniform LocalSettings settings, uniform LocalMaterial bias0,
                   uniform LocalMaterial bias1, uniform LocalMaterial bias2, uniform LocalMaterial bias3)
{
    uint index = DispatchRaysIndex().x;
#if TARGET_VULKAN
    float x = settings->firstX + settings->stepX * index;
#else
    float x = settings.firstX + settings.stepX * index;
#endif
    RayDesc ray = { float3(x, 0, -2), 0.001, float3(0, 0, 1), 100.0 };
    Payload payload = { 0 };
    TraceRay(scene, RAY_FLAG_NONE, 255, 0, 1, 0, ray, payload);
#if TARGET_VULKAN
    output[index] = payload.value + bias0->value + bias1->value + bias2->value + bias3->value;
#else
    output[index] = payload.value + bias0.value + bias1.value + bias2.value + bias3.value;
#endif
}
[shader("miss")]
void rayMiss(inout Payload payload, uniform LocalMaterial material)
{
#if TARGET_VULKAN
    payload.value = material->value;
#else
    payload.value = material.value;
#endif
}
[shader("closesthit")]
void rayHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attributes, uniform LocalMaterial material)
{
#if TARGET_VULKAN
    payload.value = material->value;
#else
    payload.value = material.value;
#endif
}
[shader("intersection")]
void rayIntersection()
{
    // This portable Slang intrinsic also maps to DXR/SPIR-V ReportHit.
    ReportHitOptix(2.0f, 0, 0u, 0u);
}

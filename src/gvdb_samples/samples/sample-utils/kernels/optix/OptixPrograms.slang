//-----------------------------------------------------------------------------
// NVIDIA(R) GVDB VOXELS
// Copyright 2017 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0
//-----------------------------------------------------------------------------
// OptiX programs of the GVDB OptiX renderer (sample-utils/OptixRenderer.cpp), in place of the OptiX 6 programs of
// the reference (optix_trace_primary.cu, optix_trace_miss.cu, optix_vol_intersect.cu, optix_trace_surface.cu,
// optix_trace_deep.cu, optix_mesh_intersect.cu). Loaded as shaders/gvdb/OptixPrograms.nvsc.
//
//   rayGeneration                     progressive accumulation (trace_primary)
//   missRadiance / missShadow         environment map (trace_miss)
//   intersectSurface / intersectDeep  custom AABB primitive per volume instance: rayCast() of GVDBRaycast.slangh
//   intersectLevelSet                 with the brick function of the mode, on the ray in the instance's index
//   intersectEmptySkip                space (object space)
//   surfaceHit                        trace_surface (reflection / refraction / shadows)
//   deepHit                           trace_deep (volume colour over what is behind)
//   meshHit                           triangles with interpolated vertex normals
//   shadowHit                         shadow attenuation (opaque surfaces, deep transmittance)
//
// Everything comes from the launch parameters, a [[vk::push_constant]] block: the scene (the IAS), the tables of
// the volumes, materials and scene instances, the camera, the output. A hit finds its scene instance by
// InstanceID(); the shader records carry no data. The intersection programs read the ray kind and depth of the
// payload, its first two words, from the payload registers. Slang has no ReportHit, no ObjectToWorld transforms of
// vectors and normals and no payload access in intersection programs for OptiX: those call the OptiX device
// functions directly.
//-----------------------------------------------------------------------------

#include "GVDBRaycast.slangh"
#include "OptixConstants.h"

static const int REFLECT_DEPTH = 1;
static const int REFRACT_DEPTH = 2;
static const int SHADOW_DEPTH = 1;

// -----------------------------------------------------------------------------
// Launch parameters (host mirrors in OptixRenderer.cpp)
// -----------------------------------------------------------------------------

// The raycast settings of a volume instance (the SCN_* values of the reference's hit record).
struct OptixVolumeInstance
{
    VDBInfo*    gvdb;
    float4*     transfer;       // TRANSFER_FUNC_SIZE entries
    float3      thresh;         // iso value, value min, value max
    int         channel;        // the rendered channel
    float3      steps;          // direct, shadow, fine step
    int         colorChannel;   // RGBA8 colour channel or CHAN_UNDEF
    float3      extinct;        // extinction, albedo
    float       epsilon;
    float3      cutoff;         // minimum value, alpha cut-off
    int         pad;
};

// Surface material (MaterialParams of the reference, OptixMaterialParams of OptixRenderer.h).
struct OptixMaterial
{
    float3      ambColor;
    float       lightWidth;     // light scatter
    float3      envColor;       // x == 1: checkerboard, cell size y, dark value z
    float       specPower;
    float3      diffColor;
    float       shadowWidth;    // shadow scatter
    float3      specColor;
    float       shadowBias;
    float3      reflColor;
    float       reflWidth;      // reflect scatter
    float3      refrColor;
    float       reflBias;
    float       refrWidth;      // refract scatter
    float       refrIor;
    float       refrAmount;
    float       refrOffset;
    float       refrBias;
    float       pad[3];
};

// One instance of the IAS (the instance ID is its index).
struct OptixInstanceData
{
    uint        kind;           // GVDB_RT_HIT_VOLUME / GVDB_RT_HIT_MESH
    uint        volumeIndex;    // volumes: index into params.volumes
    int         materialId;     // index into params.materials
    uint        numTriangles;   // meshes
    float3*     positions;      // meshes
    float3*     normals;        // meshes: interpolated vertex normals, or null
    uint3*      indices;        // meshes
};

struct OptixLaunchParams
{
    RaytracingAccelerationStructure scene;
    OptixVolumeInstance* volumes;
    OptixMaterial* materials;   // at least one
    OptixInstanceData* instances;
    float4*     accum;          // width * height, progressive accumulation
    uint*       output;         // width * height, RGBA8 (clamped)
    uint*       seeds;          // GVDB_RT_SEEDS_DIM^2 random seeds
    float4*     envmap;         // envWidth * envHeight texels, row 0 at the zenith
    float4      backclr;
    float3      eye;
    uint        numMaterials;
    // Camera rays (not normalised) through the top-left pixel corner, and the increments across the image.
    float3      rayTL;
    uint        width;
    float3      rayU;
    uint        height;
    float3      rayV;
    uint        sample;         // 0 and 1 restart the accumulation
    float3      lightPos;       // world space
    uint        envWidth;
    uint        envHeight;
    uint        pad[3];
};

[[vk::push_constant]] ConstantBuffer<OptixLaunchParams> params;

// -----------------------------------------------------------------------------
// Payload and attributes
// -----------------------------------------------------------------------------

// GVDB_RT_PAYLOAD_SIZE bytes, in payload registers, one per 32-bit word in field order.
struct RayPayload
{
    int         rtype;          // GVDB_RT_*_RAY (payload register 0)
    int         depth;          // (payload register 1)
    float3      result;
    float       length;
    float       alpha;
};

// The attributes of the volume hits (GVDB_RT_ATTRIBUTE_SIZE bytes): the hit kind is the GVDB_RT_ISECT_* mode.
struct VolumeAttributes
{
    float3      normal;         // index space
    float       tBack;          // deep volumes: the ray parameter of the back intersection
    uint        color;          // RGBA8: the colour of the colour channel, deep volumes the colour and opacity
};

// -----------------------------------------------------------------------------
// OptiX device functions Slang does not expose
// -----------------------------------------------------------------------------

void reportVolumeHit(float t, uint kind, float3 normal, float tBack, uint color)
{
    __target_switch
    {
    case cuda:
        __intrinsic_asm "optixReportIntersection($0, $1, __float_as_uint(($2).x), __float_as_uint(($2).y), __float_as_uint(($2).z), __float_as_uint($3), $4)";
    }
}

int payloadRayKind()
{
    __target_switch
    {
    case cuda:
        __intrinsic_asm "int(optixGetPayload_0())";
    }
}

int payloadDepth()
{
    __target_switch
    {
    case cuda:
        __intrinsic_asm "int(optixGetPayload_1())";
    }
}

uint attributeColor()
{
    __target_switch
    {
    case cuda:
        __intrinsic_asm "optixGetAttribute_4()";
    }
}

bool isTriangleHit()
{
    __target_switch
    {
    case cuda:
        __intrinsic_asm "optixIsTriangleHit()";
    }
}

float3 normalToWorld(float3 n)
{
    __target_switch
    {
    case cuda:
        __intrinsic_asm "optixTransformNormalFromObjectToWorldSpace($0)";
    }
}

float3 vectorToWorld(float3 v)
{
    __target_switch
    {
    case cuda:
        __intrinsic_asm "optixTransformVectorFromObjectToWorldSpace($0)";
    }
}

// -----------------------------------------------------------------------------
// Small helpers
// -----------------------------------------------------------------------------

uint packColor(float4 c)
{
    const uint4 v = uint4(saturate(c) * 255.f + 0.5f);
    return v.x | (v.y << 8) | (v.z << 16) | (v.w << 24);
}

float4 unpackColor(uint c)
{
    return float4(float(c & 0xFF), float((c >> 8) & 0xFF), float((c >> 16) & 0xFF), float((c >> 24) & 0xFF)) / 255.f;
}

// lcg() / rnd() of the reference (cuda_math.cuh): a float in [0, 1).
float rnd(inout uint prev)
{
    prev = 1664525u * prev + 1013904223u;
    return float(prev & 0x00FFFFFFu) / float(0x01000000);
}

// Jitter from the seed buffer, advancing the seed of this pixel (as the closest-hit programs of the reference did).
float3 jitterSample(bool advance = true)
{
    const uint2 li = DispatchRaysIndex().xy;
    const uint idx = (li.y & (GVDB_RT_SEEDS_DIM - 1)) * GVDB_RT_SEEDS_DIM + (li.x & (GVDB_RT_SEEDS_DIM - 1));
    uint seed = params.seeds[idx];
    const float uu = rnd(seed) - 0.5f;
    const float vv = rnd(seed) - 0.5f;
    const float ww = rnd(seed) - 0.5f;
    if (advance) params.seeds[idx] = seed;
    return float3(uu, vv, ww);
}

// Environment lookup of the reference: a texture holding the top half of the sky dome in warped spherical
// coordinates (x, y, z) = (cos(pi u) v, 1 - v, sin(pi u) v); a single colour for the sea below the horizon.
float3 sampleEnv(float3 dir)
{
    float u = atan2(dir.x, dir.z) * (1.f / 3.14159265358979323846f);   // [-1, 1]
    const float v = 1.0f - dir.y;
    if (v > 1.0f) return float3(.1f, .1f, .1f);
    if (params.envmap == nullptr || params.envWidth == 0 || params.envHeight == 0) return float3(1.f, 1.f, 1.f);

    // bilinear lookup: u wraps, v clamps (texel centres at + 0.5)
    const int w = int(params.envWidth), h = int(params.envHeight);
    u = u - floor(u);
    const float fx = u * float(w) - 0.5f;
    const float fy = saturate(v) * float(h) - 0.5f;
    const float x0f = floor(fx), y0f = floor(fy);
    const float tx = fx - x0f, ty = fy - y0f;
    int x0 = int(x0f) % w;
    if (x0 < 0) x0 += w;
    const int x1 = (x0 + 1) % w;
    const int y0 = clamp(int(y0f), 0, h - 1);
    const int y1 = clamp(int(y0f) + 1, 0, h - 1);
    const float3 top = lerp(params.envmap[y0 * w + x0].xyz, params.envmap[y0 * w + x1].xyz, tx);
    const float3 bottom = lerp(params.envmap[y1 * w + x0].xyz, params.envmap[y1 * w + x1].xyz, tx);
    return lerp(top, bottom, ty);
}

// optixu refract(): false on total internal reflection.
bool refractDir(out float3 r, float3 i, float3 n, float ior)
{
    float3 nn = n;
    float negNdotV = dot(i, nn);
    float eta;
    if (negNdotV > 0.0f) {
        eta = ior;
        nn = -n;
        negNdotV = -negNdotV;
    } else {
        eta = 1.f / ior;
    }
    const float k = 1.f - eta * eta * (1.f - negNdotV * negNdotV);
    if (k < 0.0f) {
        r = float3(0.f, 0.f, 0.f);
        return false;
    }
    r = normalize(eta * i - (eta * negNdotV + sqrt(k)) * nn);
    return true;
}

OptixMaterial materialOf(int id)
{
    const uint i = (id < 0 || uint(id) >= params.numMaterials) ? 0u : uint(id);
    return params.materials[i];
}

OptixInstanceData* currentInstance()
{
    return params.instances + InstanceID();
}

// The SCN_* values of a volume instance; the OptiX programs have no depth buffer.
RayContext volumeContext(OptixVolumeInstance* vol)
{
    RayContext ctx;
    ctx.thresh = vol->thresh;
    ctx.steps = vol->steps;
    ctx.extinct = vol->extinct;
    ctx.cutoff = vol->cutoff;
    ctx.epsilon = vol->epsilon;
    ctx.colorChannel = vol->colorChannel;
    ctx.transfer = vol->transfer;
    ctx.linearDepth = infinity();
    ctx.xform0 = float3(1, 0, 0);
    ctx.xform1 = float3(0, 1, 0);
    ctx.xform2 = float3(0, 0, 1);
    return ctx;
}

// -----------------------------------------------------------------------------
// Tracing
// -----------------------------------------------------------------------------

// TraceRay of the reference: radiance rays return the shaded colour, shadow rays the attenuation (payload alpha)
// in all three components.
float3 traceRay(float3 origin, float3 direction, int depth, int rtype, out float length)
{
    RayPayload p;
    p.rtype = rtype;
    p.depth = depth;
    p.result = float3(0.f, 0.f, 0.f);
    p.length = 0.f;
    p.alpha = 1.f;

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0f;
    ray.TMax = 1.0e16f;
    if (rtype == GVDB_RT_SHADOW_RAY)
        TraceRay(params.scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF,
                 GVDB_RT_RAY_SHADOW, GVDB_RT_RAY_TYPE_COUNT, GVDB_RT_RAY_SHADOW, ray, p);
    else
        TraceRay(params.scene, RAY_FLAG_NONE, 0xFF, GVDB_RT_RAY_RADIANCE, GVDB_RT_RAY_TYPE_COUNT,
                 GVDB_RT_RAY_RADIANCE, ray, p);
    length = p.length;
    return (rtype == GVDB_RT_SHADOW_RAY) ? float3(p.alpha, p.alpha, p.alpha) : p.result;
}

// -----------------------------------------------------------------------------
// Ray generation (trace_primary)
// -----------------------------------------------------------------------------

[shader("raygeneration")]
void rayGeneration()
{
    const uint2 idx = DispatchRaysIndex().xy;
    const uint2 dim = DispatchRaysDimensions().xy;
    if (idx.x >= params.width || idx.y >= params.height) return;

    const float2 d = float2(float(idx.x) / float(dim.x), float(idx.y) / float(dim.y));
    const float pixsize = length(params.rayU) / float(dim.x);
    const float3 jit = jitterSample(false);
    const float3 dir = normalize(d.x * params.rayU + d.y * params.rayV + params.rayTL + jit * pixsize);

    float length;
    float3 result = traceRay(params.eye, dir, 0, GVDB_RT_ANY_RAY, length);

    const uint pix = idx.y * params.width + idx.x;
    if (params.sample > 1) {
        // progressive average; samples 0 and 1 restart the accumulation
        const float4 prev = params.accum[pix];
        result = (prev.xyz * float(params.sample - 1) + result) / float(params.sample);
    }
    params.accum[pix] = float4(result, 1.f);
    const uint3 rgb = uint3(saturate(result) * 255.f);
    params.output[pix] = rgb.x | (rgb.y << 8) | (rgb.z << 16) | (255u << 24);
}

// -----------------------------------------------------------------------------
// Miss (trace_miss)
// -----------------------------------------------------------------------------

[shader("miss")]
void missRadiance(inout RayPayload prd)
{
    prd.result = sampleEnv(WorldRayDirection()) * params.backclr.xyz;
    prd.length = 1.0e10f;
}

[shader("miss")]
void missShadow(inout RayPayload prd)
{
    // unoccluded: the payload alpha stays 1
}

// -----------------------------------------------------------------------------
// Volume intersection (optix_vol_intersect.cu)
// -----------------------------------------------------------------------------

// Surface-style intersection: marches the volume in index space (the object space of the instance) with a
// normalised direction and reports the hit at the ray parameter of the (unnormalised) object ray.
void intersectVolumeSurface<B : IBrickFunc>(B brick, uint hitKind)
{
    OptixVolumeInstance* vol = params.volumes + currentInstance()->volumeIndex;
    VDBInfo* gvdb = vol->gvdb;
    if (gvdb == nullptr) return;

    const float3 orig = ObjectRayOrigin();
    const float3 odir = ObjectRayDirection();
    const float dlen = length(odir);
    if (!(dlen > 0.f)) return;
    const float3 dir = odir / dlen;

    float3 hit = float3(NOHIT, NOHIT, NOHIT);
    float3 norm = float3(0.f, 0.f, 0.f);
    float4 hclr = float4(1.f, 1.f, 1.f, 1.f);
    rayCast(gvdb, uint8_t(vol->channel), orig, dir, hit, norm, hclr, brick, volumeContext(vol));
    if (hit.z == NOHIT) return;

    const float t = length(hit - orig) / dlen;
    reportVolumeHit(t, hitKind, norm, 0.f, packColor(hclr));
}

[shader("intersection")]
void intersectSurface()
{
    intersectVolumeSurface(SurfaceTrilinearBrick(), GVDB_RT_ISECT_SURFACE);
}

[shader("intersection")]
void intersectEmptySkip()
{
    intersectVolumeSurface(EmptySkipBrick(), GVDB_RT_ISECT_EMPTYSKIP);
}

[shader("intersection")]
void intersectLevelSet()
{
    if (payloadRayKind() == GVDB_RT_REFRACT_RAY) {
        if (payloadDepth() == 2) return;
        intersectVolumeSurface(SurfaceTrilinearBrick(), GVDB_RT_ISECT_LEVELSET);
    } else {
        intersectVolumeSurface(LevelSetBrick(), GVDB_RT_ISECT_LEVELSET);
    }
}

[shader("intersection")]
void intersectDeep()
{
    if (payloadRayKind() == GVDB_RT_MESH_RAY) return;   // the "behind the volume" ray of trace_deep

    OptixVolumeInstance* vol = params.volumes + currentInstance()->volumeIndex;
    VDBInfo* gvdb = vol->gvdb;
    if (gvdb == nullptr) return;

    const float3 orig = ObjectRayOrigin();
    const float3 odir = ObjectRayDirection();
    const float dlen = length(odir);
    if (!(dlen > 0.f)) return;
    const float3 dir = odir / dlen;

    float3 hit = float3(0.f, 0.f, NOHIT);
    float3 norm = float3(0.f, 1.f, 0.f);
    float4 clr = float4(0.f, 0.f, 0.f, 1.f);
    rayCast(gvdb, uint8_t(vol->channel), orig, dir, hit, norm, clr, DeepBrick(), volumeContext(vol));
    if (hit.x == 0.f && hit.y == 0.f) return;

    // DeepBrick leaves the front / back ray parameters in hit.x / hit.y; the deep colour is the accumulated rgb
    // and the opacity.
    const float t = hit.x / dlen;
    const float tBack = hit.y / dlen;
    reportVolumeHit(t, GVDB_RT_ISECT_DEEP, norm, tBack, packColor(float4(clr.xyz, 1.f - clr.w)));
}

// -----------------------------------------------------------------------------
// Surface shading (trace_surface)
// -----------------------------------------------------------------------------

void shadeSurface(inout RayPayload prd, OptixMaterial mat, float3 n, float3 fhp, float3 raydir, float3 rayOrigin,
                  float4 deepColor)
{
    if (isnan(fhp.x) || isnan(raydir.x)) return;

    const float3 jit = jitterSample();
    const float d = length(fhp - rayOrigin);
    const float3 toLight = normalize(params.lightPos - fhp);
    float3 lightdir = normalize(toLight + jit * mat.lightWidth);
    const float ndotl = dot(n, lightdir);

    // shading
    float3 diffuse = mat.diffColor * sampleEnv(lightdir) * max(0.0f, ndotl);
    const float3 spec = mat.specColor * pow(max(0.0f, dot(n, normalize(-raydir + toLight))), mat.specPower);

    float3 reflclr = float3(0.f, 0.f, 0.f);
    float3 refrclr = float3(0.f, 0.f, 0.f);
    float shadow = 1.f;
    float dist;

    if (prd.depth < REFLECT_DEPTH && mat.reflWidth > 0.f) {
        // reflection sample
        const float3 refldir = normalize(normalize(2.f * dot(n, -raydir) * n + raydir) + jit * mat.reflWidth);
        reflclr = traceRay(fhp + refldir * mat.reflBias, refldir, prd.depth + 1, GVDB_RT_ANY_RAY, dist) *
                  mat.reflColor;
    }

    if (prd.depth < REFRACT_DEPTH && mat.refrWidth > 0.f) {
        // refraction sample
        float3 refrdir;
        if (refractDir(refrdir, raydir, n, mat.refrIor)) {
            refrdir = normalize(normalize(refrdir) + jit * mat.refrWidth);
            if (!isnan(refrdir.x)) {
                refrclr = traceRay(fhp + refrdir * mat.refrBias, refrdir, prd.depth + 1, GVDB_RT_REFRACT_RAY, dist);
                refrclr = lerp(refrclr * mat.refrAmount, mat.refrColor, min(1.0f, dist / mat.refrOffset));
            }
        }
    }

    if (prd.depth < SHADOW_DEPTH) {
        // shadow samples
        for (int i = 0; i < 2; i++) {
            lightdir = normalize(toLight + jitterSample() * mat.lightWidth);
            shadow *= traceRay(fhp + lightdir * mat.shadowBias, lightdir, prd.depth + 1, GVDB_RT_SHADOW_RAY, dist).x;
        }
    }

    if (mat.envColor.x == 1.f) {
        // checkerboard ground
        const float chk =
            ((int(floor(fhp.x / mat.envColor.y) + floor(fhp.z / mat.envColor.y)) & 1) == 0) ? 1.0f : mat.envColor.z;
        diffuse = diffuse * chk;
    }

    prd.result = (diffuse * deepColor.xyz + spec + mat.ambColor) * shadow + (reflclr + refrclr) * (shadow * 0.3f + 0.7f);
    prd.length = d;
    prd.alpha = 0.f;
}

[shader("closesthit")]
void surfaceHit(inout RayPayload prd, VolumeAttributes attributes)
{
    const float4 deepColor = unpackColor(attributes.color);

    // Front hit point offsets of the reference (index space, along the normal): surface: +2 voxels, level set: on
    // the surface, empty skip: on the brick.
    const float frontOfs = (HitKind() == GVDB_RT_ISECT_SURFACE) ? 2.f : 0.f;

    // (the object-space ray is not available in closest-hit programs: the hit point comes from the world ray, the
    // index-space offset is transformed as a vector)
    const float t = RayTCurrent();
    const float3 wdir = WorldRayDirection();
    const float3 worig = WorldRayOrigin();
    const float3 hitW = worig + t * wdir;

    float3 n = normalToWorld(attributes.normal);
    const float nl = length(n);
    n = (nl > 0.f) ? n / nl : -normalize(wdir);   // empty skipping reports no normal

    const float3 fhp = hitW + vectorToWorld(attributes.normal * frontOfs);
    shadeSurface(prd, materialOf(currentInstance()->materialId), n, fhp, wdir, worig, deepColor);
}

[shader("closesthit")]
void meshHit(inout RayPayload prd, BuiltInTriangleIntersectionAttributes attributes)
{
    OptixInstanceData* inst = currentInstance();
    const uint prim = PrimitiveIndex();
    const float2 bary = attributes.barycentrics;

    float3 nObj = float3(0.f, 1.f, 0.f);
    if (inst->indices != nullptr && inst->positions != nullptr && prim < inst->numTriangles) {
        const uint3 tri = inst->indices[prim];
        const float3 p0 = inst->positions[tri.x];
        const float3 p1 = inst->positions[tri.y];
        const float3 p2 = inst->positions[tri.z];
        nObj = cross(p1 - p0, p2 - p0);
        if (inst->normals != nullptr) {
            // interpolated vertex normals
            const float3 sn = inst->normals[tri.y] * bary.x + inst->normals[tri.z] * bary.y +
                              inst->normals[tri.x] * (1.0f - bary.x - bary.y);
            if (length(sn) > 0.f) nObj = sn;
        }
    }

    const float3 wdir = WorldRayDirection();
    const float3 worig = WorldRayOrigin();
    float3 n = normalToWorld(nObj);
    const float nl = length(n);
    n = (nl > 0.f) ? n / nl : -normalize(wdir);
    if (dot(n, wdir) > 0.f) n = -n;   // face the ray (double-sided meshes)

    const float3 hitW = worig + RayTCurrent() * wdir;
    const float scale = max(1.f, max(abs(hitW.x), max(abs(hitW.y), abs(hitW.z))));
    const float3 fhp = hitW + n * (1.0e-4f * scale);

    shadeSurface(prd, materialOf(inst->materialId), n, fhp, wdir, worig, float4(1.f, 1.f, 1.f, 1.f));
}

// -----------------------------------------------------------------------------
// Deep volume shading (trace_deep)
// -----------------------------------------------------------------------------

[shader("closesthit")]
void deepHit(inout RayPayload prd, VolumeAttributes attributes)
{
    // The intersection program already accumulated the volume along the ray: deepColor = (rgb, opacity); the hit
    // is at the first significant voxel.
    const float4 deepColor = unpackColor(attributes.color);
    const float3 wdir = WorldRayDirection();
    const float3 worig = WorldRayOrigin();

    // What lies behind the volume (polygons and surface volumes; deep volumes ignore MESH rays).
    float plen;
    const float3 bgclr = traceRay(worig, wdir, prd.depth, GVDB_RT_MESH_RAY, plen);
    const float vlen = RayTCurrent() * length(wdir);
    const float a = deepColor.w;

    prd.result = lerp(bgclr, deepColor.xyz, a);
    prd.length = vlen;
    prd.alpha = a;
}

// -----------------------------------------------------------------------------
// Shadow rays: opaque surfaces, transmittance of deep volumes
// -----------------------------------------------------------------------------

[shader("anyhit")]
void shadowHit(inout RayPayload prd, BuiltInTriangleIntersectionAttributes attributes)
{
    if (!isTriangleHit() && HitKind() == GVDB_RT_ISECT_DEEP)
        prd.alpha = 1.f - unpackColor(attributeColor()).w;   // transmittance
    else
        prd.alpha = 0.f;
    AcceptHitAndEndSearch();
}

//-----------------------------------------------------------------------------
// NVIDIA(R) GVDB VOXELS
// Copyright 2017 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0
//-----------------------------------------------------------------------------
// OptiX 9 programs of the GVDB OptiX renderer (sample-utils/OptixRenderer.cpp),
// replacing the OptiX 6 programs of the reference (optix_trace_primary.cu,
// optix_trace_miss.cu, optix_vol_intersect.cu, optix_trace_surface.cu,
// optix_trace_deep.cu, optix_mesh_intersect.cu):
//
//   __raygen__main                 progressive accumulation (trace_primary)
//   __miss__radiance / _shadow     environment map (trace_miss)
//   __exception__main
//   __intersection__volume_*       custom AABB primitive per volume instance:
//                                  rayCast() of GVDBRaycast.cuh with the brick
//                                  function of the mode, on the ray in the
//                                  instance's index space (object space)
//   __closesthit__surface          trace_surface (reflection / refraction / shadows)
//   __closesthit__deep             trace_deep (volume colour over what is behind)
//   __closesthit__mesh             triangles with interpolated vertex normals
//   __anyhit__shadow               shadow attenuation (opaque surfaces, deep transmittance)
//
// Payload: two registers holding a pointer to a RayPayload. Attributes of the
// custom primitives: normal (3), back t (1), RGBA8 colour (1); the hit kind is
// the GVDB_RT_ISECT_* mode. Scene data comes from the launch parameters
// (OptixLaunchParams.h); the per-volume raycast settings come from the volume
// instance of the current hit record (GVDBScene.cuh, OPTIX_PATHWAY).
//-----------------------------------------------------------------------------

#include <optix.h>

#include <gvdb/GVDB.cuh>
#include <gvdb/GVDBDDA.cuh>
#include <sample-utils/kernels/GVDBScene.cuh>
#include <sample-utils/kernels/GVDBRaycast.cuh>

#define REFLECT_DEPTH 1
#define REFRACT_DEPTH 2
#define SHADOW_DEPTH 1

// -----------------------------------------------------------------------------
// Payload
// -----------------------------------------------------------------------------

struct RayPayload {
    float3 result;
    float length;
    float alpha;
    int depth;
    int rtype;   // GVDB_RT_*_RAY
};

static __forceinline__ __device__ void *unpackPointer(unsigned int i0, unsigned int i1) {
    const unsigned long long uptr = static_cast<unsigned long long>(i0) << 32 | i1;
    return reinterpret_cast<void *>(uptr);
}

static __forceinline__ __device__ void packPointer(void *ptr, unsigned int &i0, unsigned int &i1) {
    const unsigned long long uptr = reinterpret_cast<unsigned long long>(ptr);
    i0 = uptr >> 32;
    i1 = uptr & 0x00000000ffffffffull;
}

static __forceinline__ __device__ RayPayload *getPayload() {
    return reinterpret_cast<RayPayload *>(unpackPointer(optixGetPayload_0(), optixGetPayload_1()));
}

// -----------------------------------------------------------------------------
// Small helpers
// -----------------------------------------------------------------------------

static __forceinline__ __device__ float saturatef(float v) { return fminf(fmaxf(v, 0.f), 1.f); }

static __forceinline__ __device__ unsigned int packColor(float4 c) {
    const unsigned int r = (unsigned int)(saturatef(c.x) * 255.f + 0.5f);
    const unsigned int g = (unsigned int)(saturatef(c.y) * 255.f + 0.5f);
    const unsigned int b = (unsigned int)(saturatef(c.z) * 255.f + 0.5f);
    const unsigned int a = (unsigned int)(saturatef(c.w) * 255.f + 0.5f);
    return r | (g << 8) | (b << 16) | (a << 24);
}

static __forceinline__ __device__ float4 unpackColor(unsigned int c) {
    return make_float4(float(c & 0xFF) / 255.f, float((c >> 8) & 0xFF) / 255.f, float((c >> 16) & 0xFF) / 255.f,
                       float((c >> 24) & 0xFF) / 255.f);
}

static __forceinline__ __device__ float3 lerpf3(float3 a, float3 b, float t) {
    return make_float3(a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z));
}

static __forceinline__ __device__ float3 neg3(float3 a) { return make_float3(-a.x, -a.y, -a.z); }

// Random numbers: lcg() / rnd() of cuda_math.cuh (the reference optix_extra_math.cuh)

// Jitter from the seed buffer, advancing the seed of this pixel (as the
// closest-hit programs of the reference did).
static __forceinline__ __device__ float3 jitterSample(bool advance = true) {
    const uint3 li = optixGetLaunchIndex();
    const unsigned int idx =
        (li.y & (GVDB_RT_SEEDS_DIM - 1)) * GVDB_RT_SEEDS_DIM + (li.x & (GVDB_RT_SEEDS_DIM - 1));
    unsigned int seed = params.seeds[idx];
    const float uu = rnd(seed) - 0.5f;
    const float vv = rnd(seed) - 0.5f;
    const float ww = rnd(seed) - 0.5f;
    if (advance) params.seeds[idx] = seed;
    return make_float3(uu, vv, ww);
}

// Environment lookup of the reference: a texture holding the top half of the
// sky dome in warped spherical coordinates (x, y, z) = (cos(pi u) v, 1 - v, sin(pi u) v);
// a single colour for the sea below the horizon.
static __forceinline__ __device__ float3 sampleEnv(float3 dir) {
    float u = atan2f(dir.x, dir.z) * M_1_PIf;   // [-1, 1]
    const float v = 1.0f - dir.y;
    if (v > 1.0f) return make_float3(.1f, .1f, .1f);
    if (params.envmap == 0 || params.envWidth == 0 || params.envHeight == 0) return make_float3(1.f, 1.f, 1.f);

    // bilinear lookup in the float4 buffer: u wraps, v clamps (texel centres at + 0.5)
    const int w = int(params.envWidth), h = int(params.envHeight);
    u = u - floorf(u);
    const float fx = u * float(w) - 0.5f;
    const float fy = saturatef(v) * float(h) - 0.5f;
    const float x0f = floorf(fx), y0f = floorf(fy);
    const float tx = fx - x0f, ty = fy - y0f;
    int x0 = int(x0f) % w;
    if (x0 < 0) x0 += w;
    const int x1 = (x0 + 1) % w;
    const int y0 = min(max(int(y0f), 0), h - 1);
    const int y1 = min(max(int(y0f) + 1, 0), h - 1);
    const float4 c00 = params.envmap[y0 * w + x0];
    const float4 c10 = params.envmap[y0 * w + x1];
    const float4 c01 = params.envmap[y1 * w + x0];
    const float4 c11 = params.envmap[y1 * w + x1];
    const float3 top = lerpf3(make_float3(c00.x, c00.y, c00.z), make_float3(c10.x, c10.y, c10.z), tx);
    const float3 bottom = lerpf3(make_float3(c01.x, c01.y, c01.z), make_float3(c11.x, c11.y, c11.z), tx);
    return lerpf3(top, bottom, ty);
}

// optixu refract(): false on total internal reflection.
static __forceinline__ __device__ bool refractDir(float3 &r, const float3 &i, const float3 &n, const float ior) {
    float3 nn = n;
    float negNdotV = dot(i, nn);
    float eta;
    if (negNdotV > 0.0f) {
        eta = ior;
        nn = neg3(n);
        negNdotV = -negNdotV;
    } else {
        eta = 1.f / ior;
    }
    const float k = 1.f - eta * eta * (1.f - negNdotV * negNdotV);
    if (k < 0.0f) {
        r = make_float3(0.f, 0.f, 0.f);
        return false;
    }
    r = normalize(eta * i - (eta * negNdotV + sqrtf(k)) * nn);
    return true;
}

// The material of a hit record; the OptixMaterialParams defaults when the
// scene has no material table.
static __forceinline__ __device__ OptixMaterial materialOf(int id) {
    if (params.materials != 0 && params.numMaterials != 0) {
        const unsigned int i = (id < 0 || (unsigned int)id >= params.numMaterials) ? 0u : (unsigned int)id;
        return params.materials[i];
    }
    OptixMaterial m;
    m.ambColor = make_float3(0.f, 0.f, 0.f);
    m.lightWidth = 0.f;
    m.envColor = make_float3(0.f, 0.f, 0.f);
    m.specPower = 400.f;
    m.diffColor = make_float3(.6f, .7f, .7f);
    m.shadowWidth = 0.f;
    m.specColor = make_float3(3.f, 3.f, 3.f);
    m.shadowBias = 0.f;
    m.reflColor = make_float3(1.f, 1.f, 1.f);
    m.reflWidth = 0.f;
    m.refrColor = make_float3(.35f, .4f, .4f);
    m.reflBias = 0.f;
    m.refrWidth = 0.f;
    m.refrIor = 1.2f;
    m.refrAmount = 10.f;
    m.refrOffset = 15.f;
    m.refrBias = 0.f;
    m.pad[0] = m.pad[1] = m.pad[2] = 0.f;
    return m;
}

// -----------------------------------------------------------------------------
// Tracing
// -----------------------------------------------------------------------------

// TraceRay of the reference: radiance rays return the shaded colour, shadow
// rays the attenuation (payload alpha) in all three components.
static __forceinline__ __device__ float3 traceRay(float3 origin, float3 direction, int depth, int rtype,
                                                  float &length) {
    RayPayload p;
    p.result = make_float3(0.f, 0.f, 0.f);
    p.length = 0.f;
    p.alpha = 1.f;
    p.depth = depth;
    p.rtype = rtype;
    unsigned int u0, u1;
    packPointer(&p, u0, u1);
    if (rtype == GVDB_RT_SHADOW_RAY) {
        optixTrace(params.traversable, origin, direction, 0.0f, 1.0e16f, 0.0f, OptixVisibilityMask(255),
                   OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT | OPTIX_RAY_FLAG_DISABLE_CLOSESTHIT, GVDB_RT_RAY_SHADOW,
                   GVDB_RT_RAY_TYPE_COUNT, GVDB_RT_RAY_SHADOW, u0, u1);
    } else {
        optixTrace(params.traversable, origin, direction, 0.0f, 1.0e16f, 0.0f, OptixVisibilityMask(255),
                   OPTIX_RAY_FLAG_NONE, GVDB_RT_RAY_RADIANCE, GVDB_RT_RAY_TYPE_COUNT, GVDB_RT_RAY_RADIANCE, u0,
                   u1);
    }
    length = p.length;
    return (rtype == GVDB_RT_SHADOW_RAY) ? make_float3(p.alpha, p.alpha, p.alpha) : p.result;
}

// -----------------------------------------------------------------------------
// Ray generation (trace_primary)
// -----------------------------------------------------------------------------

extern "C" __global__ void __raygen__main() {
    const uint3 idx = optixGetLaunchIndex();
    const uint3 dim = optixGetLaunchDimensions();
    if (idx.x >= params.width || idx.y >= params.height) return;

    const float2 d = make_float2(float(idx.x) / float(dim.x), float(idx.y) / float(dim.y));
    const float pixsize = length(params.rayU) / float(dim.x);
    const float3 jit = jitterSample(false);
    const float3 dir = normalize(d.x * params.rayU + d.y * params.rayV + params.rayTL + jit * pixsize);

    RayPayload prd;
    prd.result = make_float3(0.f, 0.f, 0.f);
    prd.length = 0.f;
    prd.alpha = 1.f;
    prd.depth = 0;
    prd.rtype = GVDB_RT_ANY_RAY;
    unsigned int u0, u1;
    packPointer(&prd, u0, u1);
    optixTrace(params.traversable, params.eye, dir, 0.0f, 1.0e16f, 0.0f, OptixVisibilityMask(255),
               OPTIX_RAY_FLAG_NONE, GVDB_RT_RAY_RADIANCE, GVDB_RT_RAY_TYPE_COUNT, GVDB_RT_RAY_RADIANCE, u0, u1);

    const unsigned int pix = idx.y * params.width + idx.x;
    float3 result = prd.result;
    if (params.sample > 1) {
        // progressive average; samples 0 and 1 restart the accumulation
        const float4 prev = params.accum[pix];
        result = (make_float3(prev.x, prev.y, prev.z) * float(params.sample - 1) + result) / float(params.sample);
    }
    params.accum[pix] = make_float4(result.x, result.y, result.z, 1.f);
    params.output[pix] = make_uchar4((unsigned char)(saturatef(result.x) * 255.f),
                                     (unsigned char)(saturatef(result.y) * 255.f),
                                     (unsigned char)(saturatef(result.z) * 255.f), 255);
}

extern "C" __global__ void __exception__main() {
    const uint3 idx = optixGetLaunchIndex();
    const int code = optixGetExceptionCode();
    printf("OptiX exception 0x%X at launch index (%u, %u)\n", code, idx.x, idx.y);
    if (idx.x < params.width && idx.y < params.height) {
        const unsigned int pix = idx.y * params.width + idx.x;
        params.accum[pix] = make_float4(0.f, 0.f, 0.f, 1.f);   // bad_color
        params.output[pix] = make_uchar4(0, 0, 0, 255);
    }
}

// -----------------------------------------------------------------------------
// Miss (trace_miss)
// -----------------------------------------------------------------------------

extern "C" __global__ void __miss__radiance() {
    RayPayload *prd = getPayload();
    const float3 dir = optixGetWorldRayDirection();
    const float4 back = SCN_BACKCLR;
    prd->result = sampleEnv(dir) * make_float3(back.x, back.y, back.z);
    prd->length = 1.0e10f;
}

extern "C" __global__ void __miss__shadow() {
    // unoccluded: payload alpha stays 1
}

// -----------------------------------------------------------------------------
// Volume intersection (optix_vol_intersect.cu)
// -----------------------------------------------------------------------------

// Brick functions as stateless functors: rayCast() calls them directly
// (OptiX forbids indirect calls through function pointers).
struct BrickSurfaceTrilinear {
    __forceinline__ __device__ void operator()(VDBInfo *gvdb, uchar chan, int nodeid, float3 t, float3 pos,
                                               float3 dir, float3 &hit, float3 &norm, float4 &clr) const {
        raySurfaceTrilinearBrick(gvdb, chan, nodeid, t, pos, dir, hit, norm, clr);
    }
};
struct BrickLevelSet {
    __forceinline__ __device__ void operator()(VDBInfo *gvdb, uchar chan, int nodeid, float3 t, float3 pos,
                                               float3 dir, float3 &hit, float3 &norm, float4 &clr) const {
        rayLevelSetBrick(gvdb, chan, nodeid, t, pos, dir, hit, norm, clr);
    }
};
struct BrickDeep {
    __forceinline__ __device__ void operator()(VDBInfo *gvdb, uchar chan, int nodeid, float3 t, float3 pos,
                                               float3 dir, float3 &hit, float3 &norm, float4 &clr) const {
        rayDeepBrick(gvdb, chan, nodeid, t, pos, dir, hit, norm, clr);
    }
};
struct BrickEmptySkip {
    __forceinline__ __device__ void operator()(VDBInfo *gvdb, uchar chan, int nodeid, float3 t, float3 pos,
                                               float3 dir, float3 &hit, float3 &norm, float4 &clr) const {
        rayEmptySkipBrick(gvdb, chan, nodeid, t, pos, dir, hit, norm, clr);
    }
};

// Surface-style intersection: marches the volume in index space (the object
// space of the instance) with a normalised direction and reports the hit at
// the ray parameter of the (unnormalised) object ray. Attributes: normal
// (index space), back t (unused), colour.
template <class BrickFn>
static __forceinline__ __device__ void intersectVolumeSurface(BrickFn brickFn, unsigned int hitKind) {
    const OptixVolumeInstance &vol = gvdbCurrentVolume();
    VDBInfo *gvdb = reinterpret_cast<VDBInfo *>(vol.gvdb);
    if (gvdb == 0) return;

    const float3 orig = optixGetObjectRayOrigin();
    const float3 odir = optixGetObjectRayDirection();
    const float dlen = length(odir);
    if (!(dlen > 0.f)) return;
    const float3 dir = odir / dlen;

    float3 hit = make_float3(NOHIT, NOHIT, NOHIT);
    float3 norm = make_float3(0.f, 0.f, 0.f);
    float4 hclr = make_float4(1.f, 1.f, 1.f, 1.f);
    rayCast(gvdb, (uchar)vol.channel, orig, dir, hit, norm, hclr, brickFn);
    if (hit.z == NOHIT) return;

    const float t = length(hit - orig) / dlen;
    optixReportIntersection(t, hitKind, __float_as_uint(norm.x), __float_as_uint(norm.y), __float_as_uint(norm.z),
                            __float_as_uint(0.f), packColor(hclr));
}

extern "C" __global__ void __intersection__volume_surface() {
    intersectVolumeSurface(BrickSurfaceTrilinear(), GVDB_RT_ISECT_SURFACE);
}

extern "C" __global__ void __intersection__volume_emptyskip() {
    intersectVolumeSurface(BrickEmptySkip(), GVDB_RT_ISECT_EMPTYSKIP);
}

extern "C" __global__ void __intersection__volume_levelset() {
    RayPayload *prd = getPayload();
    if (prd->rtype == GVDB_RT_REFRACT_RAY) {
        if (prd->depth == 2) return;
        intersectVolumeSurface(BrickSurfaceTrilinear(), GVDB_RT_ISECT_LEVELSET);
    } else {
        intersectVolumeSurface(BrickLevelSet(), GVDB_RT_ISECT_LEVELSET);
    }
}

extern "C" __global__ void __intersection__volume_deep() {
    RayPayload *prd = getPayload();
    if (prd->rtype == GVDB_RT_MESH_RAY) return;   // the "behind the volume" ray of trace_deep

    const OptixVolumeInstance &vol = gvdbCurrentVolume();
    VDBInfo *gvdb = reinterpret_cast<VDBInfo *>(vol.gvdb);
    if (gvdb == 0) return;

    const float3 orig = optixGetObjectRayOrigin();
    const float3 odir = optixGetObjectRayDirection();
    const float dlen = length(odir);
    if (!(dlen > 0.f)) return;
    const float3 dir = odir / dlen;

    float3 hit = make_float3(0.f, 0.f, NOHIT);
    float3 norm = make_float3(0.f, 1.f, 0.f);
    float4 clr = make_float4(0.f, 0.f, 0.f, 1.f);
    rayCast(gvdb, (uchar)vol.channel, orig, dir, hit, norm, clr, BrickDeep());
    if (hit.x == 0.f && hit.y == 0.f) return;

    // rayDeepBrick leaves the front / back ray parameters in hit.x / hit.y
    const float t = hit.x / dlen;
    const float tBack = hit.y / dlen;
    // deep colour: accumulated rgb, opacity
    const float4 deep = make_float4(clr.x, clr.y, clr.z, 1.f - clr.w);
    optixReportIntersection(t, GVDB_RT_ISECT_DEEP, __float_as_uint(norm.x), __float_as_uint(norm.y),
                            __float_as_uint(norm.z), __float_as_uint(tBack), packColor(deep));
}

// -----------------------------------------------------------------------------
// Surface shading (trace_surface)
// -----------------------------------------------------------------------------

static __forceinline__ __device__ void shadeSurface(RayPayload *prd, const OptixMaterial &mat, float3 n,
                                                    float3 fhp, float3 raydir, float3 rayOrigin,
                                                    float4 deepColor) {
    if (isnan(fhp.x) || isnan(raydir.x)) return;

    const float3 jit = jitterSample();
    const float d = length(fhp - rayOrigin);
    const float3 toLight = normalize(params.lightPos - fhp);
    float3 lightdir = normalize(toLight + jit * mat.lightWidth);
    const float ndotl = dot(n, lightdir);

    // shading
    float3 diffuse = mat.diffColor * sampleEnv(lightdir) * fmaxf(0.0f, ndotl);
    const float3 spec =
        mat.specColor * powf(fmaxf(0.0f, dot(n, normalize(neg3(raydir) + toLight))), mat.specPower);

    float3 reflclr = make_float3(0.f, 0.f, 0.f);
    float3 refrclr = make_float3(0.f, 0.f, 0.f);
    float shadow = 1.f;
    float dist;

    if (prd->depth < REFLECT_DEPTH && mat.reflWidth > 0.f) {
        // reflection sample
        const float3 refldir = normalize(normalize(2.f * dot(n, neg3(raydir)) * n + raydir) + jit * mat.reflWidth);
        reflclr = traceRay(fhp + refldir * mat.reflBias, refldir, prd->depth + 1, GVDB_RT_ANY_RAY, dist) *
                  mat.reflColor;
    }

    if (prd->depth < REFRACT_DEPTH && mat.refrWidth > 0.f) {
        // refraction sample
        float3 refrdir;
        if (refractDir(refrdir, raydir, n, mat.refrIor)) {
            refrdir = normalize(normalize(refrdir) + jit * mat.refrWidth);
            if (!isnan(refrdir.x)) {
                refrclr = traceRay(fhp + refrdir * mat.refrBias, refrdir, prd->depth + 1, GVDB_RT_REFRACT_RAY, dist);
                refrclr = lerpf3(refrclr * mat.refrAmount, mat.refrColor, fminf(1.0f, dist / mat.refrOffset));
            }
        }
    }

    if (prd->depth < SHADOW_DEPTH) {
        // shadow samples
        for (int i = 0; i < 2; i++) {
            lightdir = normalize(toLight + jitterSample() * mat.lightWidth);
            shadow *= traceRay(fhp + lightdir * mat.shadowBias, lightdir, prd->depth + 1, GVDB_RT_SHADOW_RAY, dist).x;
        }
    }

    if (mat.envColor.x == 1.f) {
        // checkerboard ground
        const float chk =
            ((int(floorf(fhp.x / mat.envColor.y) + floorf(fhp.z / mat.envColor.y)) & 1) == 0) ? 1.0f : mat.envColor.z;
        diffuse = diffuse * chk;
    }

    prd->result = (diffuse * make_float3(deepColor.x, deepColor.y, deepColor.z) + spec + mat.ambColor) * shadow +
                  (reflclr + refrclr) * (shadow * 0.3f + 0.7f);
    prd->length = d;
    prd->alpha = 0.f;
}

extern "C" __global__ void __closesthit__surface() {
    RayPayload *prd = getPayload();
    const OptixHitRecordData *rec = reinterpret_cast<const OptixHitRecordData *>(optixGetSbtDataPointer());

    const float3 nObj = make_float3(__uint_as_float(optixGetAttribute_0()), __uint_as_float(optixGetAttribute_1()),
                                    __uint_as_float(optixGetAttribute_2()));
    const float4 deepColor = unpackColor(optixGetAttribute_4());
    const unsigned int kind = optixGetHitKind();

    // Front hit point offsets of the reference (index space, along the normal):
    // surface: +2 voxels, level set: on the surface, empty skip: on the brick.
    const float frontOfs = (kind == GVDB_RT_ISECT_SURFACE) ? 2.f : 0.f;

    // (the object-space ray is not available in closest-hit programs: the hit
    // point comes from the world ray, the index-space offset is transformed as a vector)
    const float t = optixGetRayTmax();
    const float3 wdir = optixGetWorldRayDirection();
    const float3 worig = optixGetWorldRayOrigin();
    const float3 hitW = worig + t * wdir;

    float3 n = optixTransformNormalFromObjectToWorldSpace(nObj);
    const float nl = length(n);
    n = (nl > 0.f) ? n / nl : neg3(normalize(wdir));   // empty skipping reports no normal

    const float3 fhp = hitW + optixTransformVectorFromObjectToWorldSpace(nObj * frontOfs);
    shadeSurface(prd, materialOf(rec->materialId), n, fhp, wdir, worig, deepColor);
}

extern "C" __global__ void __closesthit__mesh() {
    RayPayload *prd = getPayload();
    const OptixHitRecordData *rec = reinterpret_cast<const OptixHitRecordData *>(optixGetSbtDataPointer());

    const unsigned int prim = optixGetPrimitiveIndex();
    const float2 bary = optixGetTriangleBarycentrics();
    const uint3 *indices = reinterpret_cast<const uint3 *>(rec->indices);
    const float3 *positions = reinterpret_cast<const float3 *>(rec->positions);
    const float3 *normals = reinterpret_cast<const float3 *>(rec->normals);

    float3 nObj = make_float3(0.f, 1.f, 0.f);
    if (indices != 0 && positions != 0 && prim < rec->numTriangles) {
        const uint3 tri = indices[prim];
        const float3 p0 = positions[tri.x];
        const float3 p1 = positions[tri.y];
        const float3 p2 = positions[tri.z];
        const float3 geoN = cross(p1 - p0, p2 - p0);
        nObj = geoN;
        if (normals != 0) {
            // interpolated vertex normals
            const float3 n0 = normals[tri.x];
            const float3 n1 = normals[tri.y];
            const float3 n2 = normals[tri.z];
            const float3 sn = n1 * bary.x + n2 * bary.y + n0 * (1.0f - bary.x - bary.y);
            if (length(sn) > 0.f) nObj = sn;
        }
    }

    const float3 wdir = optixGetWorldRayDirection();
    const float3 worig = optixGetWorldRayOrigin();
    float3 n = optixTransformNormalFromObjectToWorldSpace(nObj);
    const float nl = length(n);
    n = (nl > 0.f) ? n / nl : neg3(normalize(wdir));
    if (dot(n, wdir) > 0.f) n = neg3(n);   // face the ray (double-sided meshes)

    const float t = optixGetRayTmax();
    const float3 hitW = worig + t * wdir;
    const float scale = fmaxf(1.f, fmaxf(fabsf(hitW.x), fmaxf(fabsf(hitW.y), fabsf(hitW.z))));
    const float3 fhp = hitW + n * (1.0e-4f * scale);

    shadeSurface(prd, materialOf(rec->materialId), n, fhp, wdir, worig, make_float4(1.f, 1.f, 1.f, 1.f));
}

// -----------------------------------------------------------------------------
// Deep volume shading (trace_deep)
// -----------------------------------------------------------------------------

extern "C" __global__ void __closesthit__deep() {
    RayPayload *prd = getPayload();

    // The intersection program already accumulated the volume along the ray:
    // deepColor = (rgb, opacity); the hit is at the first significant voxel.
    const float4 deepColor = unpackColor(optixGetAttribute_4());
    const float3 wdir = optixGetWorldRayDirection();
    const float3 worig = optixGetWorldRayOrigin();

    // What lies behind the volume (polygons and surface volumes; deep volumes
    // ignore MESH rays).
    float plen;
    const float3 bgclr = traceRay(worig, wdir, prd->depth, GVDB_RT_MESH_RAY, plen);
    const float vlen = optixGetRayTmax() * length(wdir);
    const float a = deepColor.w;

    prd->result = lerpf3(bgclr, make_float3(deepColor.x, deepColor.y, deepColor.z), a);
    prd->length = vlen;
    prd->alpha = a;
}

// -----------------------------------------------------------------------------
// Shadow rays: opaque surfaces, transmittance of deep volumes
// -----------------------------------------------------------------------------

extern "C" __global__ void __anyhit__shadow() {
    RayPayload *prd = getPayload();
    if (optixIsTriangleHit()) {
        prd->alpha = 0.f;
    } else if (optixGetHitKind() == GVDB_RT_ISECT_DEEP) {
        prd->alpha = 1.f - unpackColor(optixGetAttribute_4()).w;   // transmittance
    } else {
        prd->alpha = 0.f;
    }
    optixTerminateRay();
}

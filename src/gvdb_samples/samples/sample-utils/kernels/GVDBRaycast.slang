//-----------------------------------------------------------------------------
// NVIDIA(R) GVDB VOXELS
// Copyright 2017 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0
//
// Version 1.0: Rama Hoetzlein, 5/1/2017
//-----------------------------------------------------------------------------
// The render kernels of VolRenderer (port of cuda_gvdb_raycast.cu):
// - gvdbRayDeep           - deep volume sampling
// - gvdbRaySurface*       - surface hit raycast (voxel, trilinear, tricubic)
// - gvdbRayLevelSet       - level set raycast
// - gvdbRayEmptySkip      - the bricks along the ray
// - gvdbSection3D / 2D    - cross-section renders
// - gvdbRaytrace          - a bundle of arbitrary rays
// - gvdbCompositeOver     - one render layer over another
// - gvdbCopyToSurface     - an RGBA8 buffer into a 2D texture (the screen targets of ScreenPresenter)
//
// The render kernels take (ScnInfo* scn, VDBInfo* gvdb, uint8_t chan, uint8_t4* outBuf) and run 16x16 thread
// groups over the image.
//-----------------------------------------------
#include "GVDBRaycast.slangh"

float4 performPhongShading<B : IBrickFunc>(ScnInfo* scn, VDBInfo* gvdb, uint8_t chan, float3 shit, float3 snorm,
                                           float4 sclr, B brickfunc, RayContext ctx)
{
    if (shit.z == NOHIT)   // no surface hit
        return scn->backclr;

    // phong
    float3 lightdir = normalize(scn->light_pos - shit);
    float diff = 0.9 * max(0.0f, dot(snorm, lightdir));
    float amb = 0.1f;

    // shadow ray
    if (scn->shadow_params.x > 0) {
        float3 hit2 = float3(0, 0, NOHIT);
        float4 hclr2 = float4(0, 0, 0, 1);
        float3 norm2 = float3(0, 0, 0);
        rayCast(gvdb, chan, shit + snorm * scn->shadow_params.y, lightdir, hit2, norm2, hclr2, brickfunc, ctx);   // shadow ray
        diff = (hit2.z == NOHIT ? diff : diff * (1.0 - scn->shadow_params.x));
    }
    return float4(sclr.xyz * (diff + amb), 1.0);
}

#define RENDER_ARGUMENTS uniform ScnInfo* scn, uniform VDBInfo* gvdb, uniform uint8_t chan, uniform uint8_t4* outBuf, \
    uint3 pixel : SV_DispatchThreadID

// Deep volume sampling
[shader("compute")]
[numthreads(16, 16, 1)]
void gvdbRayDeep(RENDER_ARGUMENTS)
{
    const int x = int(pixel.x), y = int(pixel.y);
    if (x >= scn->width || y >= scn->height) return;
    const RayContext ctx = makeRayContext(scn, x, y);

    float4 clr = float4(0, 0, 0, 1);
    float3 hit = float3(0, 0, NOHIT);
    float3 norm = float3(0, 0, 0);
    float3 rpos = getViewPos(scn);
    float3 rdir = getViewRay(scn, float(x + 0.5f) / float(scn->width), float(y + 0.5f) / float(scn->height));

    // ray deep sampling
    rayCast(gvdb, chan, rpos, rdir, hit, norm, clr, DeepBrick(), ctx);
    clr = float4(lerp(scn->backclr.xyz, clr.xyz, 1.0 - clr.w), 1.0 - clr.w);

    outBuf[y * scn->width + x] = toRGBA8(clr);
}

// Surface raycasts with Phong shading and shadow rays
void raySurface<B : IBrickFunc, S : IBrickFunc>(ScnInfo* scn, VDBInfo* gvdb, uint8_t chan, uint8_t4* outBuf,
                                                int x, int y, B brick, S shadowBrick)
{
    if (x >= scn->width || y >= scn->height) return;
    const RayContext ctx = makeRayContext(scn, x, y);

    float3 hit = float3(NOHIT, NOHIT, NOHIT);
    float3 norm = float3(0, 0, 0);
    float3 rpos = getViewPos(scn);
    float3 rdir = getViewRay(scn, float(x + 0.5f) / float(scn->width), float(y + 0.5f) / float(scn->height));
    float4 clr = float4(1, 1, 1, 1);

    // ray surface hit
    rayCast(gvdb, chan, rpos, rdir, hit, norm, clr, brick, ctx);
    clr = performPhongShading(scn, gvdb, chan, hit, norm, clr, shadowBrick, ctx);

    outBuf[y * scn->width + x] = toRGBA8(clr);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void gvdbRaySurfaceVoxel(RENDER_ARGUMENTS)
{
    raySurface(scn, gvdb, chan, outBuf, int(pixel.x), int(pixel.y), SurfaceVoxelBrick(), SurfaceVoxelBrick());
}

[shader("compute")]
[numthreads(16, 16, 1)]
void gvdbRaySurfaceTrilinear(RENDER_ARGUMENTS)
{
    raySurface(scn, gvdb, chan, outBuf, int(pixel.x), int(pixel.y), SurfaceTrilinearBrick(), SurfaceTrilinearBrick());
}

// (The shadow rays of the tricubic surface use the trilinear brick function, as in the reference.)
[shader("compute")]
[numthreads(16, 16, 1)]
void gvdbRaySurfaceTricubic(RENDER_ARGUMENTS)
{
    raySurface(scn, gvdb, chan, outBuf, int(pixel.x), int(pixel.y), SurfaceTricubicBrick(), SurfaceTrilinearBrick());
}

[shader("compute")]
[numthreads(16, 16, 1)]
void gvdbRayLevelSet(RENDER_ARGUMENTS)
{
    const int x = int(pixel.x), y = int(pixel.y);
    if (x >= scn->width || y >= scn->height) return;
    const RayContext ctx = makeRayContext(scn, x, y);

    float3 hit = float3(0, 0, NOHIT);
    float4 clr = float4(1, 1, 1, 1);
    float3 norm = float3(0, 0, 0);
    float3 rpos = getViewPos(scn);
    float3 rdir = getViewRay(scn, float(x + 0.5f) / float(scn->width), float(y + 0.5f) / float(scn->height));

    // Raycast Level Set
    rayCast(gvdb, chan, rpos, rdir, hit, norm, clr, LevelSetBrick(), ctx);
    clr = performPhongShading(scn, gvdb, chan, hit, norm, clr, LevelSetBrick(), ctx);

    outBuf[y * scn->width + x] = toRGBA8(clr);
}

// Empty skipping: the first brick along the ray
[shader("compute")]
[numthreads(16, 16, 1)]
void gvdbRayEmptySkip(RENDER_ARGUMENTS)
{
    const int x = int(pixel.x), y = int(pixel.y);
    if (x >= scn->width || y >= scn->height) return;
    const RayContext ctx = makeRayContext(scn, x, y);

    float4 clr = float4(1, 1, 1, 1);
    float3 hit = float3(NOHIT, NOHIT, NOHIT);
    float3 norm = float3(0, 0, 0);
    float3 rpos = getViewPos(scn);
    float3 rdir = getViewRay(scn, float(x + 0.5f) / float(scn->width), float(y + 0.5f) / float(scn->height));

    rayCast(gvdb, chan, rpos, rdir, hit, norm, clr, EmptySkipBrick(), ctx);

    if (hit.z != NOHIT) {
        clr = float4(hit * 0.01, 1);
    } else {
        clr = scn->backclr;
    }
    outBuf[y * scn->width + x] = uint8_t4(uint8_t(clr.x * 255), uint8_t(clr.y * 255), uint8_t(clr.z * 255), uint8_t(255));
}

// Raytrace a bundle of rays (64 per thread group)
[shader("compute")]
[numthreads(64, 1, 1)]
void gvdbRaytrace(uniform ScnInfo* scn, uniform VDBInfo* gvdb, uniform uint8_t chan, uniform int num_rays,
                  uniform ScnRay* rays, uniform float bias, uint3 tid : SV_DispatchThreadID)
{
    int x = int(tid.x);
    if (x >= num_rays) return;
    const RayContext ctx = makeRayContext(scn, 0, 0);

    // raytrace
    float3 hit = float3(NOHIT, NOHIT, NOHIT);
    float3 normal = rays[x].normal;
    float4 hclr = float4(1, 1, 1, 1);
    rayCast(gvdb, chan, rays[x].orig, rays[x].dir, hit, normal, hclr, SurfaceTrilinearBrick(), ctx);

    if (hit.z != NOHIT) hit -= rays[x].dir * bias;
    rays[x].hit = hit;
    rays[x].normal = normal;
}

// Render a cross section of the volume data in 3D
[shader("compute")]
[numthreads(16, 16, 1)]
void gvdbSection3D(RENDER_ARGUMENTS)
{
    const int x = int(pixel.x), y = int(pixel.y);
    if (x >= scn->width || y >= scn->height) return;
    const RayContext ctx = makeRayContext(scn, x, y);

    float4 clr = float4(1, 1, 1, 0);
    float3 norm = float3(0, 0, 0);
    float3 rdir = getViewRay(scn, float(x + 0.5f) / float(scn->width), float(y + 0.5f) / float(scn->height));

    // raytrace with cross-section plane
    float3 wpos = getViewPos(scn);
    float t = rayPlaneIntersect(wpos, rdir, scn->slice_norm, scn->slice_pnt);   // hit section plane
    if (t > 0) {                                                               // yes..
        wpos += t * rdir;                                                      // get point of surface

        float3 offs, vmin;
        uint64_t nid;
        VDBNode* node = getNodeAtPoint(gvdb, wpos, offs, vmin, nid);           // find vdb node at point
        if (node != nullptr) {
            t = getTrilinear(gvdb, chan, wpos, offs, vmin);                    // t <= voxel value
            clr = transfer(ctx, t);                                            // clr at point on surface
            if (ctx.colorChannel != CHAN_UNDEF) {
                float3 p = offs + (wpos - vmin);
                clr *= float4(float3(getColor(gvdb, uint8_t(ctx.colorChannel), p).xyz) / 255.0f, 1.0);
            }
        } else {
            t = 0;                                                             // set t=0, no voxel value found
        }
    }

    // 3D surface raytrace
    float3 hit = float3(NOHIT, NOHIT, NOHIT);
    float4 hclr = float4(1, 1, 1, 1);
    // using previous wpos (set above) to start ray, trace beyond section plane to get 3D surface hit
    rayCast(gvdb, chan, wpos, rdir, hit, norm, hclr, SurfaceTrilinearBrick(), ctx);
    if (hit.z != NOHIT) {                                                      // 3D surface hit..
        float3 lightdir = normalize(scn->light_pos - hit);
        float ds = (t > ctx.thresh.x) ? 1 : 0.8 * max(0.0f, dot(norm, lightdir));   // if voxel value on section plane is inside surface, no diffuse shading
        clr = lerp(hclr * ds, clr, clr.w);                                     // blend 3D surface with cross-section clr
    } else {
        clr = lerp(scn->backclr, clr, clr.w);                                  // no 3D hit. blend background with cross-section
    }
    outBuf[y * scn->width + x] = uint8_t4(uint8_t(clr.x * 255), uint8_t(clr.y * 255), uint8_t(clr.z * 255), uint8_t(255));
}

// Render a section of the volume data in 2D
[shader("compute")]
[numthreads(16, 16, 1)]
void gvdbSection2D(RENDER_ARGUMENTS)
{
    const int x = int(pixel.x), y = int(pixel.y);
    if (x >= scn->width || y >= scn->height) return;
    const RayContext ctx = makeRayContext(scn, x, y);

    float4 clr = float4(1, 1, 1, 0);
    float3 bgclr = float3(0, 0, 0);
    float3 wpos;
    float3 spnt = float3(float(x) * 2.0 / scn->width - 1.0, 0, float(y) * 2.0 / scn->height - 1.0);

    wpos = scn->slice_pnt + spnt * scn->slice_norm;

    // get leaf node at hit point
    float3 offs, vmin;
    uint64_t nid;
    VDBNode* node = getNodeAtPoint(gvdb, wpos, offs, vmin, nid);
    if (node == nullptr) {
        outBuf[y * scn->width + x] = uint8_t4(uint8_t(bgclr.x * 255), uint8_t(bgclr.y * 255), uint8_t(bgclr.z * 255), uint8_t(255));
        return;
    }

    // get trilinear data value
    clr = transfer(ctx, getTrilinear(gvdb, chan, wpos, offs, vmin));
    bgclr = lerp(bgclr, clr.xyz, clr.w);

    outBuf[y * scn->width + x] = uint8_t4(uint8_t(bgclr.x * 255), uint8_t(bgclr.y * 255), uint8_t(bgclr.z * 255), uint8_t(255));
}

// Composites a premultiplied RGBA8 layer over a destination RGBA8 buffer: dst = src + dst * (1 - src.a). Used by
// VolRenderer::render to stack several volume instances (the layers are rendered with a transparent black
// background, so the kernels above leave src = colour * opacity, src.a = opacity, or an opaque hit / nothing).
[shader("compute")]
[numthreads(256, 1, 1)]
void gvdbCompositeOver(uniform uint8_t4* dst, uniform uint8_t4* src, uniform int numPixels, uint3 tid : SV_DispatchThreadID)
{
    int i = int(tid.x);
    if (i >= numPixels) return;
    uint8_t4 s = src[i];
    uint8_t4 d = dst[i];
    float t = 1.0f - float(s.w) / 255.0f;
    dst[i] = uint8_t4(uint8_t(min(255.0f, float(s.x) + float(d.x) * t)),
                      uint8_t(min(255.0f, float(s.y) + float(d.y) * t)),
                      uint8_t(min(255.0f, float(s.z) + float(d.z) * t)),
                      uint8_t(min(255.0f, float(s.w) + float(d.w) * t)));
}

// A 2D R32_FLOAT texture into a float image (width x height, rows of width): a depth target of the graphics device
// as the depth buffer of the raycaster.
[shader("compute")]
[numthreads(16, 16, 1)]
void gvdbCopyFromSurfaceF(uniform RWTexture2D<float>.Handle src, uniform uint2 size, uniform float* dst,
                          uint3 pixel : SV_DispatchThreadID)
{
    if (pixel.x >= size.x || pixel.y >= size.y) return;
    dst[pixel.y * size.x + pixel.x] = src[pixel.xy];
}

// An RGBA8 image (width x height, rows of width) into a 2D RGBA8 texture.
[shader("compute")]
[numthreads(16, 16, 1)]
void gvdbCopyToSurface(uniform uint8_t4* src, uniform uint2 size, uniform RWTexture2D<uint8_t4>.Handle dst,
                       uint3 pixel : SV_DispatchThreadID)
{
    if (pixel.x >= size.x || pixel.y >= size.y) return;
    dst[pixel.xy] = src[pixel.y * size.x + pixel.x];
}

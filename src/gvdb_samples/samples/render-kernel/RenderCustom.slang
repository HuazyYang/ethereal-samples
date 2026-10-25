//-----------------------------------------------------------------------------
// NVIDIA(R) GVDB VOXELS
// Copyright 2017 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0
//-----------------------------------------------------------------------------
// render-kernel: a user raycast kernel (gRenderKernel/render_custom.cu),
// launched by VolRenderer::renderCustom with the ScnInfo of the instance.

#include "GVDBRaycast.slangh"   // VDBInfo, ScnInfo, rayCast and the brick functions

float3 reflect3(float3 i, float3 n)
{
    return i - 2.0f * n * dot(n, i);
}

// Custom raycast kernel
[shader("compute")]
[numthreads(8, 8, 1)]
void raycast_kernel(uniform ScnInfo* scn, uniform VDBInfo* gvdb, uniform uint8_t chan, uniform uint8_t4* outBuf,
                    uint3 pixel : SV_DispatchThreadID)
{
    const int x = int(pixel.x), y = int(pixel.y);
    if (x >= scn->width || y >= scn->height) return;
    const RayContext ctx = makeRayContext(scn, x, y);

    float3 hit = float3(NOHIT, NOHIT, NOHIT);
    float4 clr = float4(1, 1, 1, 1);
    float3 norm = float3(0, 0, 0);
    // Rays in the index space of the volume: the reference traced from
    // scn.campos (application space) because its grid transform was the identity.
    float3 rpos = getViewPos(scn);
    float3 rdir = normalize(getViewRay(scn, (float(x) + 0.5) / scn->width, (float(y) + 0.5) / scn->height));

    // Ray march - trace a ray into GVDB and find the closest hit point
    rayCast(gvdb, chan, rpos, rdir, hit, norm, clr, SurfaceTrilinearBrick(), ctx);

    if (hit.z != NOHIT) {
        float3 lightdir = normalize(scn->light_pos - hit);

        // Shading - custom look
        float3 eyedir = normalize(rpos - hit);
        float3 R = normalize(reflect3(eyedir, norm));   // reflection vector
        float diffuse = max(0.0f, dot(norm, lightdir));
        float refl = min(1.0f, max(0.0f, R.y));
        clr = diffuse * 0.6 + refl * float4(0, 0.3, 0.7, 1.0);
    } else {
        clr = float4(0.0, 0.0, 0.1, 1.0);
    }
    outBuf[y * scn->width + x] = uint8_t4(uint8_t(clr.x * 255), uint8_t(clr.y * 255), uint8_t(clr.z * 255), uint8_t(255));
}

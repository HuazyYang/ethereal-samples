// Shared clipping for normalized world/index-space rays. SPDX-License-Identifier: Apache-2.0
#ifndef GVDB_RAY_CLIP_SLANG
#define GVDB_RAY_CLIP_SLANG
// direction00.w == 0: march.y/z are distances along each ray.
// direction00.w == 1: march.y/z are view-Z planes measured along the normalized
// image-center ray. Divide by the per-pixel forward cosine before traversal.
float2 rayClipInterval(float3 direction) {
    float2 result=g_Parameters.march.yz;
    if(g_Parameters.direction00.w==1) {
        float3 center=g_Parameters.direction00.xyz+.5*float(g_Parameters.image.x)*g_Parameters.directionDx.xyz+
            .5*float(g_Parameters.image.y)*g_Parameters.directionDy.xyz;
        float lengthSquared=dot(center,center);
        result=float2(1,0);
        [branch] if(lengthSquared>0&&isfinite(lengthSquared)) {
            float cosine=dot(center,direction)*rsqrt(lengthSquared);
            [branch] if(cosine>0&&isfinite(cosine)) {
                float2 clipped=g_Parameters.march.yz/cosine;
                if(all(isfinite(clipped)))result=clipped;
            }
        }
    }
    return result;
}
#endif

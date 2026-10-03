//-----------------------------------------------------------------------------
// NVIDIA(R) GVDB VOXELS
// Copyright 2017 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0
//-----------------------------------------------------------------------------
// render-kernel: a user raycast kernel (gRenderKernel/render_custom.cu). The
// module has its own `scn` constant (GVDBScene.cuh), filled by
// VolRenderer::renderCustom before the launch.

#include <gvdb/GVDB.cuh>                        // GVDB data structure (VDBInfo, node traversal)
#include <gvdb/GVDBDDA.cuh>                     // GVDB DDA
#include <sample-utils/kernels/GVDBScene.cuh>   // ScnInfo / scn
#include <sample-utils/kernels/GVDBRaycast.cuh> // rayCast, brick functions

inline __host__ __device__ float3 reflect3 (float3 i, float3 n)
{
	return i - 2.0f * n * dot(n,i);
}

// Custom raycast kernel
extern "C" __global__ void raycast_kernel ( VDBInfo* gvdb, uchar chan, uchar4* outBuf )
{
	int x = blockIdx.x * blockDim.x + threadIdx.x;
	int y = blockIdx.y * blockDim.y + threadIdx.y;
	if ( x >= scn.width || y >= scn.height ) return;

	float3 hit = make_float3(NOHIT,NOHIT,NOHIT);
	float4 clr = make_float4(1,1,1,1);
	float3 norm;
	// Rays in the index space of the volume: the reference traced from
	// scn.campos (application space) because its grid transform was the identity.
	float3 rpos = getViewPos();
	float3 rdir = normalize ( getViewRay ( (float(x)+0.5)/scn.width, (float(y)+0.5)/scn.height ) );

	// Ray march - trace a ray into GVDB and find the closest hit point
	rayCast ( gvdb, chan, rpos, rdir, hit, norm, clr, raySurfaceTrilinearBrick );

	if ( hit.z != NOHIT) {
		float3 lightdir = normalize ( scn.light_pos - hit );

		// Shading - custom look
		float3 eyedir	= normalize ( rpos - hit );
		float3 R		= normalize ( reflect3 ( eyedir, norm ) );		// reflection vector
		float diffuse	= max(0.0f, dot( norm, lightdir ));
		float refl		= min(1.0f, max(0.0f, R.y ));
		clr = diffuse*0.6 + refl * make_float4(0,0.3,0.7, 1.0);

	} else {
		clr = make_float4 ( 0.0, 0.0, 0.1, 1.0 );
	}
	outBuf [ y*scn.width + x ] = make_uchar4( clr.x*255, clr.y*255, clr.z*255, 255 );
}

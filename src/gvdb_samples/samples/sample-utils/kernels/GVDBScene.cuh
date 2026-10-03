//-----------------------------------------------------------------------------
// NVIDIA(R) GVDB VOXELS
// Copyright 2017 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0
//
// Version 1.0: Rama Hoetzlein, 5/1/2017
//-----------------------------------------------------------------------------
// File: cuda_gvdb_scene.cuh
//
// CUDA Scene
// - Scene shading consts
// - Scene structure
// - Scene rays
//
// Two pathways share this header (defined by the CMake helper gvdb_add_ptx):
//   CUDA_PATHWAY   the raycast kernels: `scn` is a __constant__ ScnInfo filled
//                  by the host before each launch.
//   OPTIX_PATHWAY  the OptiX programs: `scn` is the ScnInfo inside the launch
//                  parameters, and the per-volume values (SCN_THRESH, steps,
//                  extinction, transfer function, channel ...) come from the
//                  volume instance of the current hit record, so that one
//                  launch can render several volumes with their own settings.
//-----------------------------------------------

#ifndef GVDB_SCENE_CUH
#define GVDB_SCENE_CUH

typedef unsigned char		uchar;
typedef unsigned int		uint;
typedef unsigned short		ushort;
typedef unsigned long long	uint64;
#define ALIGN(x)			__align__(x)

// Scene shading consts
#define SHADE_VOXEL		0
#define SHADE_SECTION2D	1
#define SHADE_SECTION3D	2
#define SHADE_EMPTYSKIP	3
#define SHADE_TRILINEAR	4
#define SHADE_TRICUBIC	5
#define SHADE_LEVELSET	6
#define SHADE_VOLUME	7

// GVDB Scene Info (ScnInfo) and the OptiX launch parameters: one definition
// for the CUDA kernels, the OptiX programs and the host.
#include "optix/OptixLaunchParams.h"

struct ALIGN(16) ScnRay {
	float3		hit;
	float3		normal;
	float3		orig;
	float3		dir;
	uint		clr;
	uint		pnode;
	uint		pndx;
};

#ifdef OPTIX_PATHWAY

#include <optix.h>

#define NOHIT				1.0e10f
#define BLACK				make_uchar4(0,0,0,0)

// The launch parameter block (OptixPipelineCompileOptions::pipelineLaunchParamsVariableName = "params").
extern "C" {
__constant__ OptixLaunchParams params;
}
#define scn					params.scn				// Scene Info

// The volume instance of the hit record being intersected / shaded. Valid in
// the intersection, any-hit and closest-hit programs of volume instances.
static __forceinline__ __device__ const OptixVolumeInstance& gvdbCurrentVolume()
{
	const OptixHitRecordData* rec = reinterpret_cast<const OptixHitRecordData*>(optixGetSbtDataPointer());
	return params.volumes[rec->volumeIndex];
}
#define SCN_VOL				gvdbCurrentVolume()
#define TRANSFER_FUNC		((float4*) SCN_VOL.transfer)	// Transfer Func Buffer
#define SCN_DBUF			((float*) 0)					// no depth buffer in the OptiX programs

#define SCN_EXTINCT			SCN_VOL.extinct.x
#define SCN_ALBEDO			SCN_VOL.extinct.y
#define SCN_DIRECTSTEP		SCN_VOL.steps.x
#define SCN_SHADOWSTEP		SCN_VOL.steps.y
#define SCN_FINESTEP		SCN_VOL.steps.z
#define SCN_MINVAL			SCN_VOL.cutoff.x
#define SCN_ALPHACUT		SCN_VOL.cutoff.y
#define SCN_THRESH			SCN_VOL.thresh.x
#define SCN_VMIN			SCN_VOL.thresh.y
#define SCN_VMAX			SCN_VOL.thresh.z
#define SCN_GVDB_CHANNEL	SCN_VOL.colorChannel
#define SCN_EPSILON			SCN_VOL.epsilon

#else // CUDA_PATHWAY

__constant__ float			NOHIT = 1.0e10f;
__constant__ uchar4			BLACK = {0,0,0,0};

__constant__ ScnInfo		scn;					// Scene Info
#define TRANSFER_FUNC		scn.transfer			// Transfer Func Buffer
#define SCN_DBUF			(float*) scn.dbuf		// Depth Buffer

#define SCN_EXTINCT			scn.extinct.x
#define SCN_ALBEDO			scn.extinct.y
#define SCN_DIRECTSTEP		scn.steps.x
#define SCN_SHADOWSTEP		scn.steps.y
#define SCN_FINESTEP		scn.steps.z
#define SCN_MINVAL			scn.cutoff.x
#define SCN_ALPHACUT		scn.cutoff.y
#define SCN_THRESH			scn.thresh.x
#define SCN_VMIN			scn.thresh.y
#define SCN_VMAX			scn.thresh.z
#define SCN_GVDB_CHANNEL	scn.gvdb_channel
#define SCN_EPSILON			scn.epsilon

#endif // OPTIX_PATHWAY

// Scene-wide values (identical on both pathways)
#define SCN_SHADE			scn.shading
#define SCN_SHADOWAMT		scn.shadow_params.x
#define SCN_SHADOWBIAS		scn.shadow_params.y
#define SCN_WIDTH			scn.width
#define SCN_HEIGHT			scn.height
#define SCN_BACKCLR			scn.backclr
#define SCN_SLICE_NORM		scn.slice_norm
#define SCN_SLICE_PNT		scn.slice_pnt
#define SCN_XFORM			scn.xform
#define SCN_INVXFORM		scn.invxform
#define SCN_INVXROT			scn.invxrot

#endif // GVDB_SCENE_CUH

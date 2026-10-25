#ifndef SAMPLE_UTILS_KERNELS_OPTIX_CONSTANTS_H
#define SAMPLE_UTILS_KERNELS_OPTIX_CONSTANTS_H
// Constants of the OptiX programs (kernels/optix/OptixPrograms.slang) shared with the host
// (sample-utils/OptixRenderer.cpp): plain preprocessor definitions, read by Slang and C++ alike.

// Ray types (SBT stride of the hit groups, miss program index).
#define GVDB_RT_RAY_RADIANCE 0
#define GVDB_RT_RAY_SHADOW 1
#define GVDB_RT_RAY_TYPE_COUNT 2

// Payload ray kinds of the reference (RayInfo::rtype).
#define GVDB_RT_ANY_RAY 0
#define GVDB_RT_SHADOW_RAY 1
#define GVDB_RT_VOLUME_RAY 2
#define GVDB_RT_MESH_RAY 3
#define GVDB_RT_REFRACT_RAY 4

// How a volume instance is intersected (the 'S' / 'D' / 'L' / 'E' isect of the reference AddVolume). Also the
// custom-primitive hit kind reported by the intersection programs (triangle hits have OptiX's own hit kinds).
#define GVDB_RT_ISECT_SURFACE 0
#define GVDB_RT_ISECT_DEEP 1
#define GVDB_RT_ISECT_LEVELSET 2
#define GVDB_RT_ISECT_EMPTYSKIP 3
#define GVDB_RT_ISECT_COUNT 4

// Geometry kind of a scene instance (OptixInstanceData::kind).
#define GVDB_RT_HIT_VOLUME 0
#define GVDB_RT_HIT_MESH 1

// Random seed buffer: GVDB_RT_SEEDS_DIM x GVDB_RT_SEEDS_DIM uints, indexed by (launch index & (dim - 1)).
#define GVDB_RT_SEEDS_DIM 128

// Pipeline limits: the RayPayload and VolumeAttributes structs of the programs, in bytes, and the trace depth
// (primary -> deep (mesh ray) -> surface (reflection) -> deep (mesh ray) -> surface (refraction) -> deep (mesh ray)
// -> surface (shadow)).
#define GVDB_RT_PAYLOAD_SIZE 28
#define GVDB_RT_ATTRIBUTE_SIZE 20
#define GVDB_RT_MAX_TRACE_DEPTH 8

#endif /* SAMPLE_UTILS_KERNELS_OPTIX_CONSTANTS_H */

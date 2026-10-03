#ifndef SAMPLE_UTILS_KERNELS_OPTIX_LAUNCHPARAMS_H
#define SAMPLE_UTILS_KERNELS_OPTIX_LAUNCHPARAMS_H
// Launch parameters of the OptiX programs (kernels/optix/OptixPrograms.cu),
// shared with the host (sample-utils/OptixRenderer.cpp). Plain CUDA vector
// types only. The host build does not necessarily see the CUDA toolkit
// headers, so minimal layout-identical mirrors are provided in that case.
//
// The ScnInfo block of the CUDA raycaster (GVDBScene.cuh) is defined here as
// well: on the CUDA path it is the `scn` __constant__, on the OptiX path it is
// `params.scn`. The per-volume values (threshold, steps, extinction, cut-off,
// transfer function, channel) that the reference kept in ScnInfo are per volume
// instance in this port: OptixVolumeInstance, reached by the intersection
// programs through the hit record of the instance (GVDBScene.cuh SCN_* macros).

#if defined(__CUDACC__)
#include <vector_types.h>
#define GVDB_LP_HAVE_CUDA_TYPES 1
#define GVDB_LP_ALIGN(n) __align__(n)
#else
#if defined(__has_include)
#if __has_include(<vector_types.h>)
#include <vector_types.h>
#define GVDB_LP_HAVE_CUDA_TYPES 1
#endif
#endif
#if defined(_MSC_VER)
#define GVDB_LP_ALIGN(n) __declspec(align(n))
#else
#define GVDB_LP_ALIGN(n) __attribute__((aligned(n)))
#endif
#endif

namespace gvdb_lp {
#if defined(GVDB_LP_HAVE_CUDA_TYPES)
using ::float2;
using ::float3;
using ::float4;
using ::int3;
using ::uchar4;
#else
// Layout-identical host mirrors of the CUDA vector types.
struct GVDB_LP_ALIGN(8) float2 { float x, y; };
struct float3 { float x, y, z; };
struct GVDB_LP_ALIGN(16) float4 { float x, y, z, w; };
struct int3 { int x, y, z; };
struct GVDB_LP_ALIGN(4) uchar4 { unsigned char x, y, z, w; };
#endif
}  // namespace gvdb_lp

// ---------------------------------------------------------------------------
// Constants shared by host and device
// ---------------------------------------------------------------------------

// Ray types (SBT stride of the hit groups).
#define GVDB_RT_RAY_RADIANCE 0
#define GVDB_RT_RAY_SHADOW 1
#define GVDB_RT_RAY_TYPE_COUNT 2

// Payload ray kinds of the reference (RayInfo::rtype).
#define GVDB_RT_ANY_RAY 0
#define GVDB_RT_SHADOW_RAY 1
#define GVDB_RT_VOLUME_RAY 2
#define GVDB_RT_MESH_RAY 3
#define GVDB_RT_REFRACT_RAY 4

// How a volume instance is intersected (the 'S' / 'D' / 'L' / 'E' isect of
// the reference AddVolume). Also the custom-primitive hit kind reported by the
// intersection programs (built-in triangle hits use OptiX's own hit kinds).
#define GVDB_RT_ISECT_SURFACE 0
#define GVDB_RT_ISECT_DEEP 1
#define GVDB_RT_ISECT_LEVELSET 2
#define GVDB_RT_ISECT_EMPTYSKIP 3
#define GVDB_RT_ISECT_COUNT 4

// Geometry kind of a hit record.
#define GVDB_RT_HIT_VOLUME 0
#define GVDB_RT_HIT_MESH 1

// Random seed buffer: GVDB_RT_SEEDS_DIM x GVDB_RT_SEEDS_DIM uints, indexed by
// (launch index & (dim - 1)).
#define GVDB_RT_SEEDS_DIM 128

// Attribute registers of the custom primitives (OptixPipelineCompileOptions::numAttributeValues).
#define GVDB_RT_NUM_ATTRIBUTES 5
// Payload registers: a pointer pair to the RayPayload of the trace.
#define GVDB_RT_NUM_PAYLOAD_VALUES 2

// ---------------------------------------------------------------------------
// ScnInfo: the scene block of the GVDB raycaster (cuda_gvdb_scene.cuh)
// ---------------------------------------------------------------------------
struct GVDB_LP_ALIGN(16) ScnInfo {
    int width;
    int height;
    float camnear;
    float camfar;
    gvdb_lp::float3 campos;
    gvdb_lp::float3 cams;
    gvdb_lp::float3 camu;
    gvdb_lp::float3 camv;
    gvdb_lp::float3 light_pos;
    gvdb_lp::float3 slice_pnt;
    gvdb_lp::float3 slice_norm;
    gvdb_lp::float3 shadow_params;
    gvdb_lp::float4 backclr;
    float xform[16];
    float invxform[16];
    float invxrot[16];
    float bias;
    char shading;
    char filtering;
    int frame;
    int samples;
    gvdb_lp::float3 extinct;
    gvdb_lp::float3 steps;
    gvdb_lp::float3 cutoff;
    gvdb_lp::float3 thresh;
    float epsilon;
    int gvdb_channel;
    gvdb_lp::float4 *transfer;
    char *outbuf;
    char *dbuf;
};

// ---------------------------------------------------------------------------
// OptiX launch parameters
// ---------------------------------------------------------------------------

// One volume instance of the scene (params.volumes[]). Device addresses are
// kept as integers so that the host does not need the device types.
struct GVDB_LP_ALIGN(16) OptixVolumeInstance {
    unsigned long long gvdb;        // VDBInfo* of the volume (device)
    unsigned long long transfer;    // float4[TRANSFER_FUNC_SIZE] transfer function (device)
    gvdb_lp::float3 thresh;         // iso value, value min, value max   (SCN_THRESH / SCN_VMIN / SCN_VMAX)
    int channel;                    // rendered channel
    gvdb_lp::float3 steps;          // direct, shadow, fine step         (SCN_DIRECTSTEP / SCN_SHADOWSTEP / SCN_FINESTEP)
    int colorChannel;               // RGBA8 colour channel or CHAN_UNDEF (SCN_GVDB_CHANNEL)
    gvdb_lp::float3 extinct;        // extinction, albedo                (SCN_EXTINCT / SCN_ALBEDO)
    int shading;                    // SHADE_* of the instance
    gvdb_lp::float3 cutoff;         // minimum value, alpha cut-off      (SCN_MINVAL / SCN_ALPHACUT)
    int isect;                      // GVDB_RT_ISECT_*
    float epsilon;                  // SCN_EPSILON
    int materialId;                 // index into params.materials
    int pad[2];
    float indexToWorld[12];         // row-major 3x4 (OptiX instance layout)
    float worldToIndex[12];
};

// Surface material (MaterialParams of the reference, in the layout of
// OptixMaterialParams of OptixRenderer.h).
struct GVDB_LP_ALIGN(16) OptixMaterial {
    gvdb_lp::float3 ambColor;
    float lightWidth;      // light scatter
    gvdb_lp::float3 envColor;     // x == 1: checkerboard, cell size y, dark value z
    float specPower;
    gvdb_lp::float3 diffColor;
    float shadowWidth;     // shadow scatter
    gvdb_lp::float3 specColor;
    float shadowBias;
    gvdb_lp::float3 reflColor;
    float reflWidth;       // reflect scatter
    gvdb_lp::float3 refrColor;
    float reflBias;
    float refrWidth;       // refract scatter
    float refrIor;
    float refrAmount;
    float refrOffset;
    float refrBias;
    float pad[3];
};

// User data of every hit group record (per scene instance and ray type).
struct GVDB_LP_ALIGN(16) OptixHitRecordData {
    unsigned int kind;             // GVDB_RT_HIT_VOLUME / GVDB_RT_HIT_MESH
    unsigned int volumeIndex;      // volumes: index into params.volumes
    int materialId;                // index into params.materials
    unsigned int numTriangles;     // meshes
    unsigned long long positions;  // meshes: float3* (device)
    unsigned long long normals;    // meshes: float3* or 0
    unsigned long long indices;    // meshes: uint3*
};

struct GVDB_LP_ALIGN(16) OptixLaunchParams {
    ScnInfo scn;

    const OptixVolumeInstance *volumes;
    unsigned int numVolumes;
    unsigned int pad0;
    const OptixMaterial *materials;
    unsigned int numMaterials;
    unsigned int pad1;

    // Camera: eye and the (non-normalised) ray directions through the top-left
    // pixel corner and the per-screen increments (RenderView).
    gvdb_lp::float3 eye;
    gvdb_lp::float3 rayTL;
    gvdb_lp::float3 rayU;
    gvdb_lp::float3 rayV;
    gvdb_lp::float3 lightPos;     // world space

    gvdb_lp::float4 *accum;       // width * height, progressive accumulation
    gvdb_lp::uchar4 *output;      // width * height, RGBA8 (clamped)
    unsigned int width;
    unsigned int height;

    unsigned int *seeds;          // GVDB_RT_SEEDS_DIM^2 random seeds
    unsigned int frame;
    unsigned int sample;          // 0 / 1 restart the accumulation
    unsigned int pad2;

    // Environment map: envWidth * envHeight float4 texels, row-major, row 0 at
    // the zenith (v = 0); sampled bilinearly by the programs (u wraps, v clamps).
    const gvdb_lp::float4 *envmap;
    unsigned int envWidth;
    unsigned int envHeight;

    unsigned long long traversable;   // OptixTraversableHandle of the scene IAS
    float sceneEpsilon;
    float pad3;
};

#endif /* SAMPLE_UTILS_KERNELS_OPTIX_LAUNCHPARAMS_H */

#ifndef GVDB_GPDEVICE_OPTIX_H
#define GVDB_GPDEVICE_OPTIX_H
// COM-style encapsulation of the OptiX 7+/9 host API, in the same style as the
// CUDA compute abstraction in GPDevice.h: reference-counted nvrhi objects with
// NVRHI_IID interfaces, created by an IRTDevice that is a child of a CUDA
// gp::IDevice. OptiX itself is header-only; the backend lives in
// GPDeviceOptiXImpl.cpp and is compiled only when GVDB_WITH_OPTIX is defined.
//
// Terminology follows OptiX: module (PTX / OptiX-IR), program group, pipeline,
// acceleration structure (GAS / IAS), shader binding table (SBT), launch.
#include <gvdb/GPDevice.h>

namespace donut::gp {

struct IRTDevice;
struct IRTModule;
struct IRTProgramGroup;
struct IRTPipeline;
struct IRTAccelStruct;
struct IRTShaderBindingTable;

// 64-bit traversable handle (OptixTraversableHandle).
using RTTraversableHandle = uint64_t;

enum class RTCompileOptLevel : uint8_t { Default, Level0, Level1, Level2, Level3 };
enum class RTCompileDebugLevel : uint8_t { Default, None, Minimal, Moderate, Full };

// Mirrors OptixTraversableGraphFlags.
enum RTTraversableGraphFlags : uint32_t {
    RT_TRAVERSABLE_GRAPH_ANY = 0,
    RT_TRAVERSABLE_GRAPH_SINGLE_GAS = 1u << 0,
    RT_TRAVERSABLE_GRAPH_SINGLE_LEVEL_INSTANCING = 1u << 1,
};

// Mirrors OptixExceptionFlags.
enum RTExceptionFlags : uint32_t {
    RT_EXCEPTION_NONE = 0,
    RT_EXCEPTION_STACK_OVERFLOW = 1u << 0,
    RT_EXCEPTION_TRACE_DEPTH = 1u << 1,
    RT_EXCEPTION_USER = 1u << 2,
};

// Mirrors OptixPrimitiveTypeFlags (0 = custom + triangle).
enum RTPrimitiveTypeFlags : uint32_t {
    RT_PRIMITIVE_TYPE_DEFAULT = 0,
    RT_PRIMITIVE_TYPE_CUSTOM = 1u << 0,
    RT_PRIMITIVE_TYPE_TRIANGLE = 1u << 31,
};

// Mirrors OptixGeometryFlags.
enum RTGeometryFlags : uint32_t {
    RT_GEOMETRY_FLAG_NONE = 0,
    RT_GEOMETRY_FLAG_DISABLE_ANYHIT = 1u << 0,
    RT_GEOMETRY_FLAG_REQUIRE_SINGLE_ANYHIT_CALL = 1u << 1,
};

// Mirrors OptixInstanceFlags.
enum RTInstanceFlags : uint32_t {
    RT_INSTANCE_FLAG_NONE = 0,
    RT_INSTANCE_FLAG_DISABLE_TRIANGLE_FACE_CULLING = 1u << 0,
    RT_INSTANCE_FLAG_FLIP_TRIANGLE_FACING = 1u << 1,
    RT_INSTANCE_FLAG_DISABLE_ANYHIT = 1u << 2,
    RT_INSTANCE_FLAG_ENFORCE_ANYHIT = 1u << 3,
};

// Must be identical for every module linked into one pipeline
// (OptixPipelineCompileOptions).
struct RTPipelineCompileOptions {
    bool usesMotionBlur = false;
    uint32_t traversableGraphFlags = RT_TRAVERSABLE_GRAPH_ANY;
    int numPayloadValues = 2;      // 32-bit words, [0..32]
    int numAttributeValues = 2;    // 32-bit words, [2..8]
    uint32_t exceptionFlags = RT_EXCEPTION_NONE;
    const char *launchParamsVariableName = "params";
    uint32_t primitiveTypeFlags = RT_PRIMITIVE_TYPE_DEFAULT;
};

struct RTModuleDesc {
    RTPipelineCompileOptions pipelineOptions;
    int maxRegisterCount = 0;
    RTCompileOptLevel optLevel = RTCompileOptLevel::Default;
    RTCompileDebugLevel debugLevel = RTCompileDebugLevel::Default;
};

enum class RTProgramGroupKind : uint8_t { RayGen, Miss, Exception, HitGroup, Callables };

struct RTProgramEntry {
    IRTModule *module = nullptr;
    const char *entryFunctionName = nullptr;   // e.g. "__raygen__main"; nullptr = none
};

struct RTProgramGroupDesc {
    RTProgramGroupKind kind = RTProgramGroupKind::RayGen;
    RTProgramEntry raygen;       // RayGen
    RTProgramEntry miss;         // Miss (module == nullptr -> empty miss program)
    RTProgramEntry exception;    // Exception
    struct {
        RTProgramEntry closestHit;
        RTProgramEntry anyHit;
        RTProgramEntry intersection;   // custom primitives only
    } hitgroup;
    struct {
        RTProgramEntry directCallable;
        RTProgramEntry continuationCallable;
    } callables;
};

struct RTPipelineDesc {
    RTPipelineCompileOptions compileOptions;   // must match the modules
    uint32_t maxTraceDepth = 1;
    uint32_t maxTraversableGraphDepth = 1;     // 1 = single GAS, 2 = one level of instancing
    IRTProgramGroup *const *programGroups = nullptr;
    uint32_t numProgramGroups = 0;
    RTCompileDebugLevel debugLevel = RTCompileDebugLevel::Default;
};

enum class RTGeometryType : uint8_t { Triangles, CustomPrimitives, Instances };
enum class RTIndexFormat : uint8_t { None, UInt32x3, UInt16x3 };

struct RTTrianglesInput {
    IBuffer *vertexBuffer = nullptr;   // float3 positions
    uint64_t vertexOffset = 0;
    uint32_t numVertices = 0;
    uint32_t vertexStride = 0;         // 0 = tightly packed (12 bytes)
    IBuffer *indexBuffer = nullptr;    // nullptr = non-indexed
    uint64_t indexOffset = 0;
    uint32_t numTriangles = 0;
    RTIndexFormat indexFormat = RTIndexFormat::UInt32x3;
    uint32_t geometryFlags = RT_GEOMETRY_FLAG_NONE;
    uint32_t numSbtRecords = 1;
};

// Axis aligned boxes, 6 floats each (min xyz, max xyz) = OptixAabb.
struct RTAabbsInput {
    IBuffer *aabbBuffer = nullptr;
    uint64_t aabbOffset = 0;
    uint32_t numPrimitives = 0;
    uint32_t stride = 0;               // 0 = tightly packed (24 bytes)
    uint32_t geometryFlags = RT_GEOMETRY_FLAG_NONE;
    uint32_t numSbtRecords = 1;
};

struct RTInstance {
    float transform[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};   // row-major 3x4 (OptiX layout)
    uint32_t instanceId = 0;
    uint32_t sbtOffset = 0;
    uint32_t visibilityMask = 255;
    uint32_t flags = RT_INSTANCE_FLAG_NONE;
    IRTAccelStruct *accel = nullptr;
};

struct RTAccelStructDesc {
    RTGeometryType type = RTGeometryType::Triangles;
    // Triangles: one or more inputs, each gets consecutive SBT records.
    const RTTrianglesInput *triangles = nullptr;
    uint32_t numTriangleInputs = 0;
    // Custom primitives
    const RTAabbsInput *aabbs = nullptr;
    uint32_t numAabbInputs = 0;
    // Instances
    const RTInstance *instances = nullptr;
    uint32_t numInstances = 0;

    bool allowUpdate = false;
    bool allowCompaction = true;
    bool preferFastTrace = true;
};

// One shader binding table record: 32 byte program header + user data.
struct RTSbtRecordDesc {
    IRTProgramGroup *programGroup = nullptr;
    const void *data = nullptr;
    size_t dataSize = 0;
};

struct RTShaderBindingTableDesc {
    RTSbtRecordDesc raygen;
    RTSbtRecordDesc exception;             // programGroup == nullptr: no exception record
    const RTSbtRecordDesc *miss = nullptr;
    uint32_t numMiss = 0;
    const RTSbtRecordDesc *hitgroups = nullptr;
    uint32_t numHitgroups = 0;
    const RTSbtRecordDesc *callables = nullptr;
    uint32_t numCallables = 0;
};

NVRHI_IID(IRTModule, "0c2b1b1e-7a6e-4f2b-9c0f-6a1d3b5e8f01")
struct IRTModule : public IDeviceChild {
    NVRHI_DECLARE_UUID_TRAITS(IRTModule)
    virtual const RTModuleDesc *getDesc() = 0;
};

NVRHI_IID(IRTProgramGroup, "0c2b1b1e-7a6e-4f2b-9c0f-6a1d3b5e8f02")
struct IRTProgramGroup : public IDeviceChild {
    NVRHI_DECLARE_UUID_TRAITS(IRTProgramGroup)
    virtual const RTProgramGroupDesc *getDesc() = 0;
};

NVRHI_IID(IRTPipeline, "0c2b1b1e-7a6e-4f2b-9c0f-6a1d3b5e8f03")
struct IRTPipeline : public IDeviceChild {
    NVRHI_DECLARE_UUID_TRAITS(IRTPipeline)
    virtual const RTPipelineDesc *getDesc() = 0;
};

NVRHI_IID(IRTAccelStruct, "0c2b1b1e-7a6e-4f2b-9c0f-6a1d3b5e8f04")
struct IRTAccelStruct : public IDeviceChild {
    NVRHI_DECLARE_UUID_TRAITS(IRTAccelStruct)
    virtual const RTAccelStructDesc *getDesc() = 0;
    virtual RTTraversableHandle getTraversableHandle() = 0;
    // Rebuilds (or refits, when the structure was created with allowUpdate and
    // `update` is true) with new input data of identical layout. Executes on
    // the given queue; the buffers referenced by desc must stay alive until the
    // queue has completed.
    virtual nvrhi::FRESULT rebuild(IDeviceQueue *queue, const RTAccelStructDesc &desc, bool update) = 0;
};

NVRHI_IID(IRTShaderBindingTable, "0c2b1b1e-7a6e-4f2b-9c0f-6a1d3b5e8f05")
struct IRTShaderBindingTable : public IDeviceChild {
    NVRHI_DECLARE_UUID_TRAITS(IRTShaderBindingTable)
    // Overwrites the user data of one record (the program header is kept).
    // index is within the kind's record array. Executes on the queue.
    virtual nvrhi::FRESULT updateRecordData(IDeviceQueue *queue, RTProgramGroupKind kind, uint32_t index,
                                            const void *data, size_t dataSize) = 0;
};

struct RTDeviceDesc {
    IMessageCallback *messageCallback = nullptr;   // optional; defaults to the CUDA device's callback
    bool validationMode = false;                   // OPTIX_DEVICE_CONTEXT_VALIDATION_MODE_ALL
    int logLevel = 3;                              // 0 = off .. 4 = print
};

NVRHI_IID(IRTDevice, "0c2b1b1e-7a6e-4f2b-9c0f-6a1d3b5e8f00")
struct IRTDevice : public IDeviceChild {
    NVRHI_DECLARE_UUID_TRAITS(IRTDevice)

    // data: PTX text (nul-terminated or exact size) or OptiX-IR binary.
    virtual nvrhi::FRESULT createModule(const RTModuleDesc &desc, const void *data, size_t dataSize,
                                        IRTModule **module) = 0;
    virtual nvrhi::FRESULT createProgramGroup(const RTProgramGroupDesc &desc, IRTProgramGroup **group) = 0;
    virtual nvrhi::FRESULT createPipeline(const RTPipelineDesc &desc, IRTPipeline **pipeline) = 0;
    // Builds the acceleration structure on `queue` (and compacts it when allowed).
    virtual nvrhi::FRESULT createAccelStruct(IDeviceQueue *queue, const RTAccelStructDesc &desc,
                                             IRTAccelStruct **accel) = 0;
    virtual nvrhi::FRESULT createShaderBindingTable(IDeviceQueue *queue, const RTShaderBindingTableDesc &desc,
                                                    IRTShaderBindingTable **sbt) = 0;
    // optixLaunch on the queue's stream. `params` holds the launch parameter
    // block (the __constant__ named by launchParamsVariableName).
    virtual nvrhi::FRESULT launch(IDeviceQueue *queue, IRTPipeline *pipeline, IRTShaderBindingTable *sbt,
                                  IBuffer *params, uint64_t paramsOffset, size_t paramsSize,
                                  uint32_t width, uint32_t height, uint32_t depth) = 0;
};

// Creates an OptiX device on top of a CUDA gp::IDevice (the one created by
// createCUDADevice). Returns FE_NOT_SUPPORTED when the library was built
// without OptiX or when the driver has no OptiX support.
nvrhi::FRESULT createOptiXDevice(const RTDeviceDesc &desc, IDevice *cudaDevice, IRTDevice **rtDevice);

}  // namespace donut::gp

#endif /* GVDB_GPDEVICE_OPTIX_H */

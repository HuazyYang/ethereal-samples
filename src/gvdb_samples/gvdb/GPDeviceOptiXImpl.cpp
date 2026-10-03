// Backend of GPDeviceOptiX.h (see GPDeviceOptiXImpl.h). This is the one
// translation unit that defines the OptiX function table.
#include "GPDeviceOptiXImpl.h"

#ifdef GVDB_WITH_OPTIX

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "GPDeviceCUDAImpl.h"   // default message callback of the CUDA device
#include <optix_function_table_definition.h>
#include <optix_stubs.h>
#include <optix_stack_size.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace donut::gp::rt {

#ifdef _MSC_VER
#define GP_RT_DEBUG_BREAK() __debugbreak()
#else
#define GP_RT_DEBUG_BREAK() ((void)0)
#endif

// Checks an OptiX call: logs and returns the converted error.
#define V_OPTIX(ctx, expr)                                                   \
    do {                                                                     \
        OptixResult optixResult_ = (expr);                                   \
        if (optixResult_ != OPTIX_SUCCESS)                                   \
            return (ctx).optixError(optixResult_, #expr, __FILE__, __LINE__); \
    } while (0)

// Checks a CUDA driver call.
#define V_CUDA_RT(ctx, expr)                                                 \
    do {                                                                     \
        CUresult cuResult_ = (expr);                                         \
        if (cuResult_ != CUDA_SUCCESS)                                       \
            return (ctx).cudaError(cuResult_, #expr, __FILE__, __LINE__);    \
    } while (0)

// Checks a gp call.
#define V_GP(expr)                              \
    do {                                        \
        nvrhi::FRESULT fr_ = (expr);            \
        if (NVRHI_FAILED(fr_)) return fr_;      \
    } while (0)

template <typename T1, typename T2>
static T1 *checked_cast(T2 *ptr) {
    return static_cast<T1 *>(ptr);
}

static size_t alignUp(size_t value, size_t alignment) { return (value + alignment - 1) / alignment * alignment; }

// ---------------------------------------------------------------------------
// RTContext: logging and error conversion
// ---------------------------------------------------------------------------

nvrhi::FRESULT optixResultToFRESULT(OptixResult result) {
    switch (result) {
        case OPTIX_SUCCESS:
            return nvrhi::FS_OK;
        case OPTIX_ERROR_INVALID_VALUE:
        case OPTIX_ERROR_INVALID_INPUT:
        case OPTIX_ERROR_INVALID_LAUNCH_PARAMETER:
        case OPTIX_ERROR_INVALID_POINTER:
        case OPTIX_ERROR_INVALID_ENTRY_FUNCTION_OPTIONS:
            return nvrhi::FE_INVALID_ARGS;
        case OPTIX_ERROR_HOST_OUT_OF_MEMORY:
        case OPTIX_ERROR_DEVICE_OUT_OF_MEMORY:
        case OPTIX_ERROR_PIPELINE_OUT_OF_CONSTANT_MEMORY:
            return nvrhi::FE_OUT_OF_MEMORY;
        case OPTIX_ERROR_NOT_SUPPORTED:
        case OPTIX_ERROR_UNSUPPORTED_ABI_VERSION:
        case OPTIX_ERROR_FUNCTION_TABLE_SIZE_MISMATCH:
        case OPTIX_ERROR_LIBRARY_NOT_FOUND:
        case OPTIX_ERROR_ENTRY_SYMBOL_NOT_FOUND:
        case OPTIX_ERROR_NOT_COMPATIBLE:
            return nvrhi::FE_UNSUPPORTED;
        case OPTIX_ERROR_FILE_IO_ERROR:
        case OPTIX_ERROR_DISK_CACHE_INVALID_PATH:
            return nvrhi::FE_NOT_FOUND;
        default:
            return nvrhi::FE_GENERIC_ERROR;
    }
}

static void vlogRT(IMessageCallback *callback, MessageSeverity severity, const char *file, int line,
                   const char *fmt, va_list ap) {
    const char *logRep;
    switch (severity) {
        case MessageSeverity::Error:
            logRep = "[Error]";
            break;
        case MessageSeverity::Fatal:
            logRep = "[Fatal]";
            break;
        case MessageSeverity::Warning:
            logRep = "[Warning]";
            break;
        case MessageSeverity::Info:
        default:
            logRep = "[Info]";
            break;
    }

    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(nullptr, 0, fmt, ap2);
    va_end(ap2);
    std::string s1;
    s1.resize(size_t(std::max(n, 0)) + 1);
    vsnprintf(s1.data(), s1.size(), fmt, ap);
    s1.resize(size_t(std::max(n, 0)));

    std::string s2;
    if (file) {
        n = snprintf(nullptr, 0, "[donut::gp::optix]%s %s:%d: %s\n", logRep, file, line, s1.c_str());
        s2.resize(size_t(n) + 1);
        snprintf(s2.data(), s2.size(), "[donut::gp::optix]%s %s:%d: %s\n", logRep, file, line, s1.c_str());
    } else {
        n = snprintf(nullptr, 0, "[donut::gp::optix]%s %s\n", logRep, s1.c_str());
        s2.resize(size_t(n) + 1);
        snprintf(s2.data(), s2.size(), "[donut::gp::optix]%s %s\n", logRep, s1.c_str());
    }

    if (callback)
        callback->message(severity, s2.c_str());
    else
        fputs(s2.c_str(), stderr);
}

void RTContext::log(MessageSeverity severity, const char *file, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlogRT(msgCallback.Get(), severity, file, line, fmt, ap);
    va_end(ap);
}

nvrhi::FRESULT RTContext::optixError(OptixResult result, const char *expr, const char *file, int line) {
    log(MessageSeverity::Error, file, line, "[OptiX] %s: %s (%s)", expr, optixGetErrorString(result),
        optixGetErrorName(result));
    GP_RT_DEBUG_BREAK();
    return optixResultToFRESULT(result);
}

nvrhi::FRESULT RTContext::cudaError(CUresult result, const char *expr, const char *file, int line) {
    const char *errDesc = "?", *errName = "?";
    cuGetErrorString(result, &errDesc);
    cuGetErrorName(result, &errName);
    log(MessageSeverity::Error, file, line, "[CUDA] %s: %s (%s)", expr, errDesc, errName);
    GP_RT_DEBUG_BREAK();
    return result == CUDA_ERROR_OUT_OF_MEMORY ? nvrhi::FE_OUT_OF_MEMORY : nvrhi::FE_GENERIC_ERROR;
}

void RTContext::optixCompileLog(const char *what, const char *logText, size_t logSize, OptixResult result) {
    if (!logText || logSize <= 1 || logText[0] == 0) return;
    // the compiler log is informational unless the call failed
    const MessageSeverity severity = (result == OPTIX_SUCCESS) ? MessageSeverity::Info : MessageSeverity::Error;
    if (severity == MessageSeverity::Info && logLevel < 4) return;
    log(severity, nullptr, 0, "[OptiX] %s log:\n%s", what, logText);
}

static void optixLogCallback(unsigned int level, const char *tag, const char *message, void *cbdata) {
    RTContext *ctx = static_cast<RTContext *>(cbdata);
    MessageSeverity severity;
    switch (level) {
        case 1:
            severity = MessageSeverity::Fatal;
            break;
        case 2:
            severity = MessageSeverity::Error;
            break;
        case 3:
            severity = MessageSeverity::Warning;
            break;
        default:
            severity = MessageSeverity::Info;
            break;
    }
    ctx->log(severity, nullptr, 0, "[OptiX][%s] %s", tag ? tag : "", message ? message : "");
}

// ---------------------------------------------------------------------------
// ContextScope
// ---------------------------------------------------------------------------

ContextScope::ContextScope(CUcontext context) { cuCtxPushCurrent(context); }

ContextScope::~ContextScope() {
    CUcontext dummy = nullptr;
    cuCtxPopCurrent(&dummy);
}

// ---------------------------------------------------------------------------
// Enum conversions
// ---------------------------------------------------------------------------

static OptixCompileOptimizationLevel toOptix(RTCompileOptLevel level) {
    switch (level) {
        case RTCompileOptLevel::Level0:
            return OPTIX_COMPILE_OPTIMIZATION_LEVEL_0;
        case RTCompileOptLevel::Level1:
            return OPTIX_COMPILE_OPTIMIZATION_LEVEL_1;
        case RTCompileOptLevel::Level2:
            return OPTIX_COMPILE_OPTIMIZATION_LEVEL_2;
        case RTCompileOptLevel::Level3:
            return OPTIX_COMPILE_OPTIMIZATION_LEVEL_3;
        case RTCompileOptLevel::Default:
        default:
            return OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
    }
}

static OptixCompileDebugLevel toOptix(RTCompileDebugLevel level) {
    switch (level) {
        case RTCompileDebugLevel::None:
            return OPTIX_COMPILE_DEBUG_LEVEL_NONE;
        case RTCompileDebugLevel::Minimal:
            return OPTIX_COMPILE_DEBUG_LEVEL_MINIMAL;
        case RTCompileDebugLevel::Moderate:
            return OPTIX_COMPILE_DEBUG_LEVEL_MODERATE;
        case RTCompileDebugLevel::Full:
            return OPTIX_COMPILE_DEBUG_LEVEL_FULL;
        case RTCompileDebugLevel::Default:
        default:
            return OPTIX_COMPILE_DEBUG_LEVEL_DEFAULT;
    }
}

// The flag enums of GPDeviceOptiX.h mirror the OptiX values bit for bit.
static OptixPipelineCompileOptions toOptix(const RTPipelineCompileOptions &options, const std::string &paramsName) {
    OptixPipelineCompileOptions pco = {};
    pco.usesMotionBlur = options.usesMotionBlur ? 1 : 0;
    pco.traversableGraphFlags = options.traversableGraphFlags;
    pco.numPayloadValues = options.numPayloadValues;
    pco.numAttributeValues = options.numAttributeValues;
    pco.exceptionFlags = options.exceptionFlags;
    pco.pipelineLaunchParamsVariableName = paramsName.c_str();
    pco.usesPrimitiveTypeFlags = options.primitiveTypeFlags;
    pco.allowOpacityMicromaps = 0;
    pco.allowClusteredGeometry = 0;
    return pco;
}

static OptixProgramGroupKind toOptix(RTProgramGroupKind kind) {
    switch (kind) {
        case RTProgramGroupKind::RayGen:
            return OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
        case RTProgramGroupKind::Miss:
            return OPTIX_PROGRAM_GROUP_KIND_MISS;
        case RTProgramGroupKind::Exception:
            return OPTIX_PROGRAM_GROUP_KIND_EXCEPTION;
        case RTProgramGroupKind::HitGroup:
            return OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        case RTProgramGroupKind::Callables:
        default:
            return OPTIX_PROGRAM_GROUP_KIND_CALLABLES;
    }
}

static CUdeviceptr deviceAddress(IBuffer *buffer, uint64_t offset = 0) {
    if (!buffer) return 0;
    const CUdeviceptr base = (CUdeviceptr)buffer->getNativeHandle();
    return base ? base + offset : 0;
}

// ---------------------------------------------------------------------------
// RTDevice
// ---------------------------------------------------------------------------

RTDevice::RTDevice(IDevice *cudaDevice, const RTDeviceDesc &desc) : m_cudaDevice(cudaDevice) {
    m_ctx.cudaDevice = cudaDevice;
    m_ctx.cuContext = (CUcontext)cudaDevice->getNativeHandle();
    m_ctx.logLevel = desc.logLevel;
    if (desc.messageCallback) {
        m_ctx.msgCallback = desc.messageCallback;
    } else {
        // the CUDA device's callback (the device is the one created by createCUDADevice)
        auto *device = static_cast<cuda::Device *>(cudaDevice);
        m_ctx.msgCallback = device->m_context.msgCallback;
    }
}

RTDevice::~RTDevice() {
    if (m_ctx.optixContext) {
        ContextScope scope(m_ctx.cuContext);
        optixDeviceContextDestroy(m_ctx.optixContext);
        m_ctx.optixContext = nullptr;
    }
}

nvrhi::FRESULT RTDevice::init(const RTDeviceDesc &desc) {
    ContextScope scope(m_ctx.cuContext);

    OptixDeviceContextOptions options = {};
    options.logCallbackFunction = &optixLogCallback;
    options.logCallbackData = &m_ctx;
    options.logCallbackLevel = std::max(0, std::min(desc.logLevel, 4));
    options.validationMode =
        desc.validationMode ? OPTIX_DEVICE_CONTEXT_VALIDATION_MODE_ALL : OPTIX_DEVICE_CONTEXT_VALIDATION_MODE_OFF;

    V_OPTIX(m_ctx, optixDeviceContextCreate(m_ctx.cuContext, &options, &m_ctx.optixContext));
    return nvrhi::FS_OK;
}

nvrhi::FRESULT RTDevice::createModule(const RTModuleDesc &desc, const void *data, size_t dataSize,
                                      IRTModule **module) {
    if (!module || !data || dataSize == 0) return nvrhi::FE_INVALID_ARGS;
    auto *obj = MAKE_RC_OBJ(RTModule, this, desc);
    nvrhi::FRESULT fr = obj->init(data, dataSize);
    if (NVRHI_FAILED(fr)) {
        obj->Release();
        *module = nullptr;
        return fr;
    }
    *module = obj;
    return nvrhi::FS_OK;
}

nvrhi::FRESULT RTDevice::createProgramGroup(const RTProgramGroupDesc &desc, IRTProgramGroup **group) {
    if (!group) return nvrhi::FE_INVALID_ARGS;
    auto *obj = MAKE_RC_OBJ(RTProgramGroup, this, desc);
    nvrhi::FRESULT fr = obj->init();
    if (NVRHI_FAILED(fr)) {
        obj->Release();
        *group = nullptr;
        return fr;
    }
    *group = obj;
    return nvrhi::FS_OK;
}

nvrhi::FRESULT RTDevice::createPipeline(const RTPipelineDesc &desc, IRTPipeline **pipeline) {
    if (!pipeline || !desc.programGroups || desc.numProgramGroups == 0) return nvrhi::FE_INVALID_ARGS;
    auto *obj = MAKE_RC_OBJ(RTPipeline, this, desc);
    nvrhi::FRESULT fr = obj->init();
    if (NVRHI_FAILED(fr)) {
        obj->Release();
        *pipeline = nullptr;
        return fr;
    }
    *pipeline = obj;
    return nvrhi::FS_OK;
}

nvrhi::FRESULT RTDevice::createAccelStruct(IDeviceQueue *queue, const RTAccelStructDesc &desc,
                                           IRTAccelStruct **accel) {
    if (!accel || !queue) return nvrhi::FE_INVALID_ARGS;
    auto *obj = MAKE_RC_OBJ(RTAccelStruct, this, desc);
    nvrhi::FRESULT fr = obj->build(queue, desc, false);
    if (NVRHI_FAILED(fr)) {
        obj->Release();
        *accel = nullptr;
        return fr;
    }
    *accel = obj;
    return nvrhi::FS_OK;
}

nvrhi::FRESULT RTDevice::createShaderBindingTable(IDeviceQueue *queue, const RTShaderBindingTableDesc &desc,
                                                  IRTShaderBindingTable **sbt) {
    if (!sbt || !queue) return nvrhi::FE_INVALID_ARGS;
    auto *obj = MAKE_RC_OBJ(RTShaderBindingTable, this);
    nvrhi::FRESULT fr = obj->init(queue, desc);
    if (NVRHI_FAILED(fr)) {
        obj->Release();
        *sbt = nullptr;
        return fr;
    }
    *sbt = obj;
    return nvrhi::FS_OK;
}

nvrhi::FRESULT RTDevice::launch(IDeviceQueue *queue, IRTPipeline *_pipeline, IRTShaderBindingTable *_sbt,
                                IBuffer *params, uint64_t paramsOffset, size_t paramsSize, uint32_t width,
                                uint32_t height, uint32_t depth) {
    if (!queue || !_pipeline || !_sbt) return nvrhi::FE_INVALID_ARGS;
    auto *pipeline = checked_cast<RTPipeline>(_pipeline);
    auto *sbt = checked_cast<RTShaderBindingTable>(_sbt);
    if (!pipeline->m_pipeline || sbt->m_sbt.raygenRecord == 0) return nvrhi::FE_INVALID_ARGS;
    if (params && paramsOffset + paramsSize > params->getDesc()->byteSize) return nvrhi::FE_INVALID_ARGS;

    ContextScope scope(m_ctx.cuContext);
    CUstream stream = (CUstream)queue->getNativeHandle();
    const CUdeviceptr paramsPtr = params ? deviceAddress(params, paramsOffset) : 0;
    V_OPTIX(m_ctx, optixLaunch(pipeline->m_pipeline, stream, paramsPtr, params ? paramsSize : 0, &sbt->m_sbt,
                               width, height, depth));
    return nvrhi::FS_OK;
}

// ---------------------------------------------------------------------------
// RTModule
// ---------------------------------------------------------------------------

RTModule::RTModule(RTDevice *device, const RTModuleDesc &desc) : RTChild<IRTModule>(device), m_desc(desc) {
    m_launchParamsName = desc.pipelineOptions.launchParamsVariableName ? desc.pipelineOptions.launchParamsVariableName
                                                                       : "params";
    m_desc.pipelineOptions.launchParamsVariableName = m_launchParamsName.c_str();
}

RTModule::~RTModule() {
    if (m_module) {
        ContextScope scope(ctx().cuContext);
        optixModuleDestroy(m_module);
        m_module = nullptr;
    }
}

nvrhi::FRESULT RTModule::init(const void *data, size_t dataSize) {
    RTContext &c = ctx();
    ContextScope scope(c.cuContext);

    OptixModuleCompileOptions mco = {};
    mco.maxRegisterCount = m_desc.maxRegisterCount > 0 ? m_desc.maxRegisterCount : OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
    mco.optLevel = toOptix(m_desc.optLevel);
    mco.debugLevel = toOptix(m_desc.debugLevel);

    const OptixPipelineCompileOptions pco = toOptix(m_desc.pipelineOptions, m_launchParamsName);

    // PTX text may carry its terminating nul; OptiX accepts either.
    char log[4096];
    size_t logSize = sizeof(log);
    log[0] = 0;
    OptixResult result = optixModuleCreate(c.optixContext, &mco, &pco, static_cast<const char *>(data), dataSize, log,
                                           &logSize, &m_module);
    c.optixCompileLog("module", log, logSize, result);
    if (result != OPTIX_SUCCESS) {
        m_module = nullptr;
        return c.optixError(result, "optixModuleCreate", __FILE__, __LINE__);
    }
    return nvrhi::FS_OK;
}

// ---------------------------------------------------------------------------
// RTProgramGroup
// ---------------------------------------------------------------------------

RTProgramGroup::RTProgramGroup(RTDevice *device, const RTProgramGroupDesc &desc)
    : RTChild<IRTProgramGroup>(device), m_desc(desc) {
    // Copy the entry names and keep the modules alive; the desc keeps pointing
    // at our storage.
    RTProgramEntry *entries[] = {&m_desc.raygen,
                                 &m_desc.miss,
                                 &m_desc.exception,
                                 &m_desc.hitgroup.closestHit,
                                 &m_desc.hitgroup.anyHit,
                                 &m_desc.hitgroup.intersection,
                                 &m_desc.callables.directCallable,
                                 &m_desc.callables.continuationCallable};
    m_names.reserve(std::size(entries));
    for (RTProgramEntry *entry : entries) {
        if (entry->module) m_modules.push_back(nvrhi::AutoPtr<IRTModule>(entry->module));
        if (entry->entryFunctionName) {
            m_names.emplace_back(entry->entryFunctionName);
            entry->entryFunctionName = m_names.back().c_str();
        }
    }
}

RTProgramGroup::~RTProgramGroup() {
    if (m_group) {
        ContextScope scope(ctx().cuContext);
        optixProgramGroupDestroy(m_group);
        m_group = nullptr;
    }
}

static OptixModule optixModuleOf(const RTProgramEntry &entry) {
    return entry.module ? checked_cast<RTModule>(entry.module)->m_module : nullptr;
}

static const char *entryNameOf(const RTProgramEntry &entry) {
    return entry.module ? entry.entryFunctionName : nullptr;
}

nvrhi::FRESULT RTProgramGroup::init() {
    RTContext &c = ctx();
    ContextScope scope(c.cuContext);

    OptixProgramGroupDesc pgd = {};
    pgd.kind = toOptix(m_desc.kind);
    pgd.flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
    switch (m_desc.kind) {
        case RTProgramGroupKind::RayGen:
            if (!m_desc.raygen.module || !m_desc.raygen.entryFunctionName) return nvrhi::FE_INVALID_ARGS;
            pgd.raygen.module = optixModuleOf(m_desc.raygen);
            pgd.raygen.entryFunctionName = entryNameOf(m_desc.raygen);
            break;
        case RTProgramGroupKind::Miss:
            pgd.miss.module = optixModuleOf(m_desc.miss);
            pgd.miss.entryFunctionName = entryNameOf(m_desc.miss);
            break;
        case RTProgramGroupKind::Exception:
            if (!m_desc.exception.module || !m_desc.exception.entryFunctionName) return nvrhi::FE_INVALID_ARGS;
            pgd.exception.module = optixModuleOf(m_desc.exception);
            pgd.exception.entryFunctionName = entryNameOf(m_desc.exception);
            break;
        case RTProgramGroupKind::HitGroup:
            pgd.hitgroup.moduleCH = optixModuleOf(m_desc.hitgroup.closestHit);
            pgd.hitgroup.entryFunctionNameCH = entryNameOf(m_desc.hitgroup.closestHit);
            pgd.hitgroup.moduleAH = optixModuleOf(m_desc.hitgroup.anyHit);
            pgd.hitgroup.entryFunctionNameAH = entryNameOf(m_desc.hitgroup.anyHit);
            pgd.hitgroup.moduleIS = optixModuleOf(m_desc.hitgroup.intersection);
            pgd.hitgroup.entryFunctionNameIS = entryNameOf(m_desc.hitgroup.intersection);
            break;
        case RTProgramGroupKind::Callables:
            pgd.callables.moduleDC = optixModuleOf(m_desc.callables.directCallable);
            pgd.callables.entryFunctionNameDC = entryNameOf(m_desc.callables.directCallable);
            pgd.callables.moduleCC = optixModuleOf(m_desc.callables.continuationCallable);
            pgd.callables.entryFunctionNameCC = entryNameOf(m_desc.callables.continuationCallable);
            break;
    }

    OptixProgramGroupOptions options = {};
    char log[4096];
    size_t logSize = sizeof(log);
    log[0] = 0;
    OptixResult result = optixProgramGroupCreate(c.optixContext, &pgd, 1, &options, log, &logSize, &m_group);
    c.optixCompileLog("program group", log, logSize, result);
    if (result != OPTIX_SUCCESS) {
        m_group = nullptr;
        return c.optixError(result, "optixProgramGroupCreate", __FILE__, __LINE__);
    }
    return nvrhi::FS_OK;
}

// ---------------------------------------------------------------------------
// RTPipeline
// ---------------------------------------------------------------------------

RTPipeline::RTPipeline(RTDevice *device, const RTPipelineDesc &desc) : RTChild<IRTPipeline>(device), m_desc(desc) {
    m_launchParamsName = desc.compileOptions.launchParamsVariableName ? desc.compileOptions.launchParamsVariableName
                                                                      : "params";
    m_desc.compileOptions.launchParamsVariableName = m_launchParamsName.c_str();
    m_programGroups.reserve(desc.numProgramGroups);
    m_programGroupPtrs.reserve(desc.numProgramGroups);
    for (uint32_t i = 0; i < desc.numProgramGroups; ++i) {
        m_programGroups.push_back(nvrhi::AutoPtr<IRTProgramGroup>(desc.programGroups[i]));
        m_programGroupPtrs.push_back(desc.programGroups[i]);
    }
    m_desc.programGroups = m_programGroupPtrs.data();
}

RTPipeline::~RTPipeline() {
    if (m_pipeline) {
        ContextScope scope(ctx().cuContext);
        optixPipelineDestroy(m_pipeline);
        m_pipeline = nullptr;
    }
}

nvrhi::FRESULT RTPipeline::init() {
    RTContext &c = ctx();
    ContextScope scope(c.cuContext);

    std::vector<OptixProgramGroup> groups;
    groups.reserve(m_programGroupPtrs.size());
    for (IRTProgramGroup *g : m_programGroupPtrs) {
        if (!g) return nvrhi::FE_INVALID_ARGS;
        groups.push_back(checked_cast<RTProgramGroup>(g)->m_group);
    }

    const OptixPipelineCompileOptions pco = toOptix(m_desc.compileOptions, m_launchParamsName);
    OptixPipelineLinkOptions plo = {};
    plo.maxTraceDepth = m_desc.maxTraceDepth;
    // Note: OptiX 9 has no link-time debug level; the modules' debug level applies.

    char log[4096];
    size_t logSize = sizeof(log);
    log[0] = 0;
    OptixResult result = optixPipelineCreate(c.optixContext, &pco, &plo, groups.data(), (unsigned int)groups.size(),
                                             log, &logSize, &m_pipeline);
    c.optixCompileLog("pipeline", log, logSize, result);
    if (result != OPTIX_SUCCESS) {
        m_pipeline = nullptr;
        return c.optixError(result, "optixPipelineCreate", __FILE__, __LINE__);
    }

    // Stack sizes: accumulate the groups' requirements, size for the trace
    // depth and the traversable graph depth.
    OptixStackSizes stackSizes = {};
    for (OptixProgramGroup g : groups) V_OPTIX(c, optixUtilAccumulateStackSizes(g, &stackSizes, m_pipeline));

    unsigned int dcStackFromTraversal = 0, dcStackFromState = 0, continuationStack = 0;
    V_OPTIX(c, optixUtilComputeStackSizes(&stackSizes, m_desc.maxTraceDepth, 0, 0, &dcStackFromTraversal,
                                          &dcStackFromState, &continuationStack));
    V_OPTIX(c, optixPipelineSetStackSize(m_pipeline, dcStackFromTraversal, dcStackFromState, continuationStack,
                                         std::max(1u, m_desc.maxTraversableGraphDepth)));
    return nvrhi::FS_OK;
}

// ---------------------------------------------------------------------------
// RTAccelStruct
// ---------------------------------------------------------------------------

RTAccelStruct::RTAccelStruct(RTDevice *device, const RTAccelStructDesc &desc) : RTChild<IRTAccelStruct>(device) {
    storeDesc(desc);
}

RTAccelStruct::~RTAccelStruct() {}

void RTAccelStruct::storeDesc(const RTAccelStructDesc &desc) {
    m_desc = desc;
    m_triangles.assign(desc.triangles, desc.triangles + (desc.triangles ? desc.numTriangleInputs : 0));
    m_aabbs.assign(desc.aabbs, desc.aabbs + (desc.aabbs ? desc.numAabbInputs : 0));
    m_instances.assign(desc.instances, desc.instances + (desc.instances ? desc.numInstances : 0));
    m_desc.triangles = m_triangles.empty() ? nullptr : m_triangles.data();
    m_desc.numTriangleInputs = (uint32_t)m_triangles.size();
    m_desc.aabbs = m_aabbs.empty() ? nullptr : m_aabbs.data();
    m_desc.numAabbInputs = (uint32_t)m_aabbs.size();
    m_desc.instances = m_instances.empty() ? nullptr : m_instances.data();
    m_desc.numInstances = (uint32_t)m_instances.size();

    // keep the referenced buffers / child structures alive with this object
    m_references.clear();
    for (auto &t : m_triangles) {
        if (t.vertexBuffer) m_references.push_back(nvrhi::AutoPtr<nvrhi::IObject>(t.vertexBuffer));
        if (t.indexBuffer) m_references.push_back(nvrhi::AutoPtr<nvrhi::IObject>(t.indexBuffer));
    }
    for (auto &a : m_aabbs)
        if (a.aabbBuffer) m_references.push_back(nvrhi::AutoPtr<nvrhi::IObject>(a.aabbBuffer));
    for (auto &i : m_instances)
        if (i.accel) m_references.push_back(nvrhi::AutoPtr<nvrhi::IObject>(i.accel));
}

nvrhi::FRESULT RTAccelStruct::rebuild(IDeviceQueue *queue, const RTAccelStructDesc &desc, bool update) {
    if (!queue) return nvrhi::FE_INVALID_ARGS;
    if (desc.type != m_desc.type) return nvrhi::FE_INVALID_ARGS;
    return build(queue, desc, update);
}

nvrhi::FRESULT RTAccelStruct::build(IDeviceQueue *queue, const RTAccelStructDesc &desc, bool update) {
    RTContext &c = ctx();
    ContextScope scope(c.cuContext);
    CUstream stream = (CUstream)queue->getNativeHandle();

    // an update (refit) needs a previous build with allowUpdate
    update = update && m_desc.allowUpdate && desc.allowUpdate && m_outputBuffer && m_handle != 0;
    const bool allowUpdate = update ? true : desc.allowUpdate;
    storeDesc(desc);
    m_desc.allowUpdate = allowUpdate;

    // ---- build inputs ----
    std::vector<OptixBuildInput> inputs;
    std::vector<CUdeviceptr> pointers;     // stable storage for pointer arrays
    std::vector<unsigned int> flags;       // stable storage for per-record flags
    size_t numInputs = 0;
    switch (m_desc.type) {
        case RTGeometryType::Triangles:
            numInputs = m_triangles.size();
            break;
        case RTGeometryType::CustomPrimitives:
            numInputs = m_aabbs.size();
            break;
        case RTGeometryType::Instances:
            numInputs = 1;
            break;
    }
    if (m_desc.type != RTGeometryType::Instances && numInputs == 0) return nvrhi::FE_INVALID_ARGS;
    inputs.reserve(numInputs);
    pointers.reserve(numInputs);
    flags.reserve(numInputs);

    std::vector<OptixInstance> instances;
    switch (m_desc.type) {
        case RTGeometryType::Triangles: {
            for (const RTTrianglesInput &t : m_triangles) {
                if (!t.vertexBuffer || t.numVertices == 0) return nvrhi::FE_INVALID_ARGS;
                pointers.push_back(deviceAddress(t.vertexBuffer, t.vertexOffset));
                flags.push_back(t.geometryFlags);
                OptixBuildInput bi = {};
                bi.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
                bi.triangleArray.vertexBuffers = &pointers.back();
                bi.triangleArray.numVertices = t.numVertices;
                bi.triangleArray.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
                bi.triangleArray.vertexStrideInBytes = t.vertexStride;
                if (t.indexBuffer && t.indexFormat != RTIndexFormat::None) {
                    bi.triangleArray.indexBuffer = deviceAddress(t.indexBuffer, t.indexOffset);
                    bi.triangleArray.numIndexTriplets = t.numTriangles;
                    bi.triangleArray.indexFormat = t.indexFormat == RTIndexFormat::UInt16x3
                                                       ? OPTIX_INDICES_FORMAT_UNSIGNED_SHORT3
                                                       : OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
                    bi.triangleArray.indexStrideInBytes = 0;
                } else {
                    bi.triangleArray.indexBuffer = 0;
                    bi.triangleArray.numIndexTriplets = 0;
                    bi.triangleArray.indexFormat = OPTIX_INDICES_FORMAT_NONE;
                }
                bi.triangleArray.flags = &flags.back();
                bi.triangleArray.numSbtRecords = std::max(1u, t.numSbtRecords);
                bi.triangleArray.transformFormat = OPTIX_TRANSFORM_FORMAT_NONE;
                inputs.push_back(bi);
            }
            break;
        }
        case RTGeometryType::CustomPrimitives: {
            for (const RTAabbsInput &a : m_aabbs) {
                if (!a.aabbBuffer || a.numPrimitives == 0) return nvrhi::FE_INVALID_ARGS;
                pointers.push_back(deviceAddress(a.aabbBuffer, a.aabbOffset));
                flags.push_back(a.geometryFlags);
                OptixBuildInput bi = {};
                bi.type = OPTIX_BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES;
                bi.customPrimitiveArray.aabbBuffers = &pointers.back();
                bi.customPrimitiveArray.numPrimitives = a.numPrimitives;
                bi.customPrimitiveArray.strideInBytes = a.stride;
                bi.customPrimitiveArray.flags = &flags.back();
                bi.customPrimitiveArray.numSbtRecords = std::max(1u, a.numSbtRecords);
                inputs.push_back(bi);
            }
            break;
        }
        case RTGeometryType::Instances: {
            instances.resize(m_instances.size());
            for (size_t i = 0; i < m_instances.size(); ++i) {
                const RTInstance &src = m_instances[i];
                OptixInstance &dst = instances[i];
                memset(&dst, 0, sizeof(dst));
                memcpy(dst.transform, src.transform, sizeof(dst.transform));
                dst.instanceId = src.instanceId;
                dst.sbtOffset = src.sbtOffset;
                dst.visibilityMask = src.visibilityMask;
                dst.flags = src.flags;
                dst.traversableHandle = src.accel ? src.accel->getTraversableHandle() : 0;
            }
            const size_t bytes = std::max<size_t>(1, instances.size()) * sizeof(OptixInstance);
            if (!m_instanceBuffer || m_instanceBuffer->getDesc()->byteSize < bytes) {
                BufferDesc bd;
                bd.byteSize = bytes;
                m_instanceBuffer = nullptr;
                V_GP(c.cudaDevice->createBuffer(bd, &m_instanceBuffer));
            }
            if (!instances.empty())
                V_GP(queue->writeBuffer(m_instanceBuffer, instances.data(), instances.size() * sizeof(OptixInstance), 0));

            OptixBuildInput bi = {};
            bi.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
            bi.instanceArray.instances = instances.empty() ? 0 : deviceAddress(m_instanceBuffer);
            bi.instanceArray.numInstances = (unsigned int)instances.size();
            bi.instanceArray.instanceStride = 0;
            inputs.push_back(bi);
            break;
        }
    }

    // ---- sizes ----
    OptixAccelBuildOptions options = {};
    options.buildFlags = OPTIX_BUILD_FLAG_NONE;
    if (m_desc.allowUpdate) options.buildFlags |= OPTIX_BUILD_FLAG_ALLOW_UPDATE;
    if (m_desc.allowCompaction) options.buildFlags |= OPTIX_BUILD_FLAG_ALLOW_COMPACTION;
    options.buildFlags |= m_desc.preferFastTrace ? OPTIX_BUILD_FLAG_PREFER_FAST_TRACE : OPTIX_BUILD_FLAG_PREFER_FAST_BUILD;
    options.operation = update ? OPTIX_BUILD_OPERATION_UPDATE : OPTIX_BUILD_OPERATION_BUILD;
    options.motionOptions.numKeys = 1;

    OptixAccelBufferSizes sizes = {};
    V_OPTIX(c, optixAccelComputeMemoryUsage(c.optixContext, &options, inputs.data(), (unsigned int)inputs.size(),
                                            &sizes));

    const size_t tempSize = update ? sizes.tempUpdateSizeInBytes : sizes.tempSizeInBytes;
    nvrhi::AutoPtr<IBuffer> tempBuffer;
    if (tempSize > 0) {
        BufferDesc bd;
        bd.byteSize = tempSize;
        V_GP(c.cudaDevice->createBuffer(bd, &tempBuffer));
    }

    nvrhi::AutoPtr<IBuffer> outputBuffer = m_outputBuffer;
    size_t outputSize = m_outputSize;
    if (!update) {
        BufferDesc bd;
        bd.byteSize = std::max<size_t>(sizes.outputSizeInBytes, 1);
        outputBuffer = nullptr;
        V_GP(c.cudaDevice->createBuffer(bd, &outputBuffer));
        outputSize = sizes.outputSizeInBytes;
    } else if (outputSize < sizes.outputSizeInBytes) {
        // a refit must fit the original structure; fall back to a full build
        return build(queue, desc, false);
    }

    // ---- build (+ compaction) ----
    const bool compact = !update && m_desc.allowCompaction;
    nvrhi::AutoPtr<IBuffer> compactedSizeBuffer;
    OptixAccelEmitDesc emit = {};
    if (compact) {
        BufferDesc bd;
        bd.byteSize = sizeof(uint64_t);
        V_GP(c.cudaDevice->createBuffer(bd, &compactedSizeBuffer));
        emit.result = deviceAddress(compactedSizeBuffer);
        emit.type = OPTIX_PROPERTY_TYPE_COMPACTED_SIZE;
    }

    OptixTraversableHandle handle = update ? m_handle : 0;
    V_OPTIX(c, optixAccelBuild(c.optixContext, stream, &options, inputs.data(), (unsigned int)inputs.size(),
                               tempBuffer ? deviceAddress(tempBuffer) : 0, tempSize, deviceAddress(outputBuffer),
                               outputSize, &handle, compact ? &emit : nullptr, compact ? 1 : 0));
    // The temp buffer (and, for compaction, the result) are consumed on the
    // stream: wait before they go out of scope.
    V_CUDA_RT(c, cuStreamSynchronize(stream));

    if (compact) {
        uint64_t compactedSize = 0;
        V_CUDA_RT(c, cuMemcpyDtoH(&compactedSize, emit.result, sizeof(compactedSize)));
        if (compactedSize > 0 && compactedSize < outputSize) {
            nvrhi::AutoPtr<IBuffer> compactedBuffer;
            BufferDesc bd;
            bd.byteSize = compactedSize;
            V_GP(c.cudaDevice->createBuffer(bd, &compactedBuffer));
            V_OPTIX(c, optixAccelCompact(c.optixContext, stream, handle, deviceAddress(compactedBuffer),
                                         compactedSize, &handle));
            V_CUDA_RT(c, cuStreamSynchronize(stream));
            outputBuffer = compactedBuffer;
            outputSize = compactedSize;
        }
    }

    m_outputBuffer = outputBuffer;
    m_outputSize = outputSize;
    m_handle = handle;
    return nvrhi::FS_OK;
}

// ---------------------------------------------------------------------------
// RTShaderBindingTable
// ---------------------------------------------------------------------------

RTShaderBindingTable::RTShaderBindingTable(RTDevice *device) : RTChild<IRTShaderBindingTable>(device) {}

RTShaderBindingTable::~RTShaderBindingTable() {}

RTShaderBindingTable::RecordArray *RTShaderBindingTable::arrayOf(RTProgramGroupKind kind) {
    switch (kind) {
        case RTProgramGroupKind::RayGen:
            return &m_raygen;
        case RTProgramGroupKind::Exception:
            return &m_exception;
        case RTProgramGroupKind::Miss:
            return &m_miss;
        case RTProgramGroupKind::HitGroup:
            return &m_hitgroups;
        case RTProgramGroupKind::Callables:
            return &m_callables;
    }
    return nullptr;
}

nvrhi::FRESULT RTShaderBindingTable::packRecords(IDeviceQueue *queue, RecordArray &array,
                                                 const RTSbtRecordDesc *records, uint32_t count) {
    RTContext &c = ctx();
    array.host.clear();
    array.buffer = nullptr;
    array.stride = 0;
    array.count = 0;
    if (!records || count == 0) return nvrhi::FS_OK;

    size_t maxData = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!records[i].programGroup) return nvrhi::FE_INVALID_ARGS;
        maxData = std::max(maxData, records[i].dataSize);
    }
    array.stride = alignUp(OPTIX_SBT_RECORD_HEADER_SIZE + maxData, OPTIX_SBT_RECORD_ALIGNMENT);
    array.count = count;
    array.host.assign(array.stride * count, 0);

    for (uint32_t i = 0; i < count; ++i) {
        uint8_t *record = array.host.data() + i * array.stride;
        auto *group = checked_cast<RTProgramGroup>(records[i].programGroup);
        V_OPTIX(c, optixSbtRecordPackHeader(group->m_group, record));
        if (records[i].data && records[i].dataSize)
            memcpy(record + OPTIX_SBT_RECORD_HEADER_SIZE, records[i].data, records[i].dataSize);
        m_programGroups.push_back(nvrhi::AutoPtr<IRTProgramGroup>(records[i].programGroup));
    }

    BufferDesc bd;
    bd.byteSize = array.host.size();
    V_GP(c.cudaDevice->createBuffer(bd, &array.buffer));
    V_GP(queue->writeBuffer(array.buffer, array.host.data(), array.host.size(), 0));
    return nvrhi::FS_OK;
}

nvrhi::FRESULT RTShaderBindingTable::init(IDeviceQueue *queue, const RTShaderBindingTableDesc &desc) {
    RTContext &c = ctx();
    ContextScope scope(c.cuContext);

    if (!desc.raygen.programGroup) return nvrhi::FE_INVALID_ARGS;
    V_GP(packRecords(queue, m_raygen, &desc.raygen, 1));
    if (desc.exception.programGroup) V_GP(packRecords(queue, m_exception, &desc.exception, 1));
    V_GP(packRecords(queue, m_miss, desc.miss, desc.numMiss));
    V_GP(packRecords(queue, m_hitgroups, desc.hitgroups, desc.numHitgroups));
    V_GP(packRecords(queue, m_callables, desc.callables, desc.numCallables));

    memset(&m_sbt, 0, sizeof(m_sbt));
    m_sbt.raygenRecord = deviceAddress(m_raygen.buffer);
    m_sbt.exceptionRecord = m_exception.buffer ? deviceAddress(m_exception.buffer) : 0;
    m_sbt.missRecordBase = m_miss.buffer ? deviceAddress(m_miss.buffer) : 0;
    m_sbt.missRecordStrideInBytes = (unsigned int)m_miss.stride;
    m_sbt.missRecordCount = m_miss.count;
    m_sbt.hitgroupRecordBase = m_hitgroups.buffer ? deviceAddress(m_hitgroups.buffer) : 0;
    m_sbt.hitgroupRecordStrideInBytes = (unsigned int)m_hitgroups.stride;
    m_sbt.hitgroupRecordCount = m_hitgroups.count;
    m_sbt.callablesRecordBase = m_callables.buffer ? deviceAddress(m_callables.buffer) : 0;
    m_sbt.callablesRecordStrideInBytes = (unsigned int)m_callables.stride;
    m_sbt.callablesRecordCount = m_callables.count;
    return nvrhi::FS_OK;
}

nvrhi::FRESULT RTShaderBindingTable::updateRecordData(IDeviceQueue *queue, RTProgramGroupKind kind, uint32_t index,
                                                      const void *data, size_t dataSize) {
    RecordArray *array = arrayOf(kind);
    if (!queue || !array || index >= array->count || !array->buffer) return nvrhi::FE_INVALID_ARGS;
    if (dataSize == 0) return nvrhi::FS_OK;
    if (!data || OPTIX_SBT_RECORD_HEADER_SIZE + dataSize > array->stride) return nvrhi::FE_INVALID_ARGS;

    const size_t offset = index * array->stride + OPTIX_SBT_RECORD_HEADER_SIZE;
    memcpy(array->host.data() + offset, data, dataSize);
    return queue->writeBuffer(array->buffer, data, dataSize, offset);
}

}  // namespace donut::gp::rt

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

namespace donut::gp {

nvrhi::FRESULT createOptiXDevice(const RTDeviceDesc &desc, IDevice *cudaDevice, IRTDevice **rtDevice) {
    if (!cudaDevice || !rtDevice) return nvrhi::FE_INVALID_ARGS;
    *rtDevice = nullptr;

    // optixInit() loads the OptiX library of the display driver; once per process.
    static std::once_flag s_initOnce;
    static OptixResult s_initResult = OPTIX_SUCCESS;
    std::call_once(s_initOnce, []() { s_initResult = optixInit(); });

    auto *device = MAKE_RC_OBJ(rt::RTDevice, cudaDevice, desc);
    if (s_initResult != OPTIX_SUCCESS) {
        device->m_ctx.log(MessageSeverity::Error, __FILE__, __LINE__, "[OptiX] optixInit failed: %s (%s)",
                          optixGetErrorString(s_initResult), optixGetErrorName(s_initResult));
        device->Release();
        return nvrhi::FE_UNSUPPORTED;
    }

    nvrhi::FRESULT fr = device->init(desc);
    if (NVRHI_FAILED(fr)) {
        device->Release();
        return fr;
    }
    *rtDevice = device;
    return nvrhi::FS_OK;
}

}  // namespace donut::gp

#else  // !GVDB_WITH_OPTIX

namespace donut::gp {

nvrhi::FRESULT createOptiXDevice(const RTDeviceDesc &desc, IDevice *cudaDevice, IRTDevice **rtDevice) {
    (void)desc;
    (void)cudaDevice;
    if (rtDevice) *rtDevice = nullptr;
    return nvrhi::FE_UNSUPPORTED;
}

}  // namespace donut::gp

#endif  // GVDB_WITH_OPTIX

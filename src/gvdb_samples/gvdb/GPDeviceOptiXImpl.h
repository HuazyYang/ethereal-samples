#ifndef GVDB_GPDEVICE_OPTIX_IMPL_H
#define GVDB_GPDEVICE_OPTIX_IMPL_H
// Backend of GPDeviceOptiX.h on the OptiX 7+/9 host API. Compiled only when
// GVDB_WITH_OPTIX is defined (CMake sets it when the SDK was found); without
// it, createOptiXDevice() reports FE_UNSUPPORTED (GPDeviceOptiXImpl.cpp).
//
// Every object keeps a reference to its RTDevice, which keeps the CUDA
// gp::IDevice alive; all device memory is gp buffers so lifetimes follow
// AutoPtr. OptiX / CUDA calls run with the CUDA device's context current.
#include <gvdb/GPDeviceOptiX.h>

#ifdef GVDB_WITH_OPTIX

#include <cuda.h>
#include <optix.h>
#include <nvrhi/core/autoptr.h>
#include <string>
#include <vector>

namespace donut::gp::rt {

struct RTDevice;

// Shared state of an OptiX device: the CUDA context it was created on, the
// OptiX context and the message callback.
struct RTContext {
    IDevice *cudaDevice = nullptr;   // owned by RTDevice::m_cudaDevice
    CUcontext cuContext = nullptr;
    OptixDeviceContext optixContext = nullptr;
    nvrhi::AutoPtr<IMessageCallback> msgCallback;
    int logLevel = 3;

    void log(MessageSeverity severity, const char *file, int line, const char *fmt, ...);
    // Logs an OptixResult (name and description) and converts it to an FRESULT.
    nvrhi::FRESULT optixError(OptixResult result, const char *expr, const char *file, int line);
    // Logs a CUresult and converts it.
    nvrhi::FRESULT cudaError(CUresult result, const char *expr, const char *file, int line);
    // Logs the compile / link log of a module, program group or pipeline.
    void optixCompileLog(const char *what, const char *logText, size_t logSize, OptixResult result);
};

// Makes a CUDA context current for the lifetime of the scope.
struct ContextScope {
    explicit ContextScope(CUcontext context);
    ~ContextScope();
    ContextScope(const ContextScope &) = delete;
    ContextScope &operator=(const ContextScope &) = delete;
};

nvrhi::FRESULT optixResultToFRESULT(OptixResult result);

struct RTDevice : public nvrhi::ObjectImpl<IRTDevice> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(RTDevice)
    NVRHI_IMPLEMENTS_INTERFACE(IRTDevice)
    NVRHI_IMPLEMENTS_INTERFACE(IDeviceChild)
    NVRHI_END_INTERFACE_TABLE()

    IDevice *getDevice() override { return m_cudaDevice.Get(); }

    nvrhi::FRESULT createModule(const RTModuleDesc &desc, const void *data, size_t dataSize,
                                IRTModule **module) override;
    nvrhi::FRESULT createProgramGroup(const RTProgramGroupDesc &desc, IRTProgramGroup **group) override;
    nvrhi::FRESULT createPipeline(const RTPipelineDesc &desc, IRTPipeline **pipeline) override;
    nvrhi::FRESULT createAccelStruct(IDeviceQueue *queue, const RTAccelStructDesc &desc,
                                     IRTAccelStruct **accel) override;
    nvrhi::FRESULT createShaderBindingTable(IDeviceQueue *queue, const RTShaderBindingTableDesc &desc,
                                            IRTShaderBindingTable **sbt) override;
    nvrhi::FRESULT launch(IDeviceQueue *queue, IRTPipeline *pipeline, IRTShaderBindingTable *sbt, IBuffer *params,
                          uint64_t paramsOffset, size_t paramsSize, uint32_t width, uint32_t height,
                          uint32_t depth) override;

    // Implements
    RTDevice(IDevice *cudaDevice, const RTDeviceDesc &desc);
    ~RTDevice();
    // Creates the OptiX device context on the CUDA device's context.
    nvrhi::FRESULT init(const RTDeviceDesc &desc);

    nvrhi::AutoPtr<IDevice> m_cudaDevice;
    RTContext m_ctx;
};

// T derives from IDeviceChild.
template <typename T>
struct RTChild : public nvrhi::ObjectImpl<T> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(RTChild)
    NVRHI_IMPLEMENTS_INTERFACE(T)
    NVRHI_IMPLEMENTS_INTERFACE(IDeviceChild)
    NVRHI_END_INTERFACE_TABLE()

    IDevice *getDevice() override final { return m_rtDevice->m_cudaDevice.Get(); }

    // Implements
    explicit RTChild(RTDevice *device) : m_rtDevice(device) {}
    RTContext &ctx() { return m_rtDevice->m_ctx; }

    nvrhi::AutoPtr<RTDevice> m_rtDevice;
};

struct RTModule : public RTChild<IRTModule> {
    NVRHI_INHERIT_INTERFACE_TABLE()

    const RTModuleDesc *getDesc() override { return &m_desc; }

    // Implements
    RTModule(RTDevice *device, const RTModuleDesc &desc);
    ~RTModule();
    nvrhi::FRESULT init(const void *data, size_t dataSize);

    RTModuleDesc m_desc;
    std::string m_launchParamsName;   // storage of m_desc.pipelineOptions.launchParamsVariableName
    OptixModule m_module = nullptr;
};

struct RTProgramGroup : public RTChild<IRTProgramGroup> {
    NVRHI_INHERIT_INTERFACE_TABLE()

    const RTProgramGroupDesc *getDesc() override { return &m_desc; }

    // Implements
    RTProgramGroup(RTDevice *device, const RTProgramGroupDesc &desc);
    ~RTProgramGroup();
    nvrhi::FRESULT init();

    RTProgramGroupDesc m_desc;
    // Storage behind the desc's pointers (modules kept alive, names copied).
    std::vector<nvrhi::AutoPtr<IRTModule>> m_modules;
    std::vector<std::string> m_names;
    OptixProgramGroup m_group = nullptr;
};

struct RTPipeline : public RTChild<IRTPipeline> {
    NVRHI_INHERIT_INTERFACE_TABLE()

    const RTPipelineDesc *getDesc() override { return &m_desc; }

    // Implements
    RTPipeline(RTDevice *device, const RTPipelineDesc &desc);
    ~RTPipeline();
    nvrhi::FRESULT init();

    RTPipelineDesc m_desc;
    std::string m_launchParamsName;
    std::vector<nvrhi::AutoPtr<IRTProgramGroup>> m_programGroups;
    std::vector<IRTProgramGroup *> m_programGroupPtrs;   // m_desc.programGroups
    OptixPipeline m_pipeline = nullptr;
};

struct RTAccelStruct : public RTChild<IRTAccelStruct> {
    NVRHI_INHERIT_INTERFACE_TABLE()

    const RTAccelStructDesc *getDesc() override { return &m_desc; }
    RTTraversableHandle getTraversableHandle() override { return m_handle; }
    nvrhi::FRESULT rebuild(IDeviceQueue *queue, const RTAccelStructDesc &desc, bool update) override;

    // Implements
    RTAccelStruct(RTDevice *device, const RTAccelStructDesc &desc);
    ~RTAccelStruct();
    nvrhi::FRESULT build(IDeviceQueue *queue, const RTAccelStructDesc &desc, bool update);
    void storeDesc(const RTAccelStructDesc &desc);

    RTAccelStructDesc m_desc;
    // Storage behind m_desc's arrays and the objects they reference.
    std::vector<RTTrianglesInput> m_triangles;
    std::vector<RTAabbsInput> m_aabbs;
    std::vector<RTInstance> m_instances;
    std::vector<nvrhi::AutoPtr<nvrhi::IObject>> m_references;

    nvrhi::AutoPtr<IBuffer> m_outputBuffer;     // the acceleration structure
    nvrhi::AutoPtr<IBuffer> m_instanceBuffer;   // OptixInstance array (instances)
    OptixTraversableHandle m_handle = 0;
    size_t m_outputSize = 0;
};

struct RTShaderBindingTable : public RTChild<IRTShaderBindingTable> {
    NVRHI_INHERIT_INTERFACE_TABLE()

    nvrhi::FRESULT updateRecordData(IDeviceQueue *queue, RTProgramGroupKind kind, uint32_t index, const void *data,
                                    size_t dataSize) override;

    // Implements
    struct RecordArray {
        std::vector<uint8_t> host;     // packed records (header + data), host mirror
        nvrhi::AutoPtr<IBuffer> buffer;
        size_t stride = 0;
        uint32_t count = 0;
    };

    RTShaderBindingTable(RTDevice *device);
    ~RTShaderBindingTable();
    nvrhi::FRESULT init(IDeviceQueue *queue, const RTShaderBindingTableDesc &desc);
    nvrhi::FRESULT packRecords(IDeviceQueue *queue, RecordArray &array, const RTSbtRecordDesc *records,
                               uint32_t count);
    RecordArray *arrayOf(RTProgramGroupKind kind);

    RecordArray m_raygen;
    RecordArray m_exception;
    RecordArray m_miss;
    RecordArray m_hitgroups;
    RecordArray m_callables;
    std::vector<nvrhi::AutoPtr<IRTProgramGroup>> m_programGroups;
    OptixShaderBindingTable m_sbt = {};
};

}  // namespace donut::gp::rt

#endif /* GVDB_WITH_OPTIX */

#endif /* GVDB_GPDEVICE_OPTIX_IMPL_H */

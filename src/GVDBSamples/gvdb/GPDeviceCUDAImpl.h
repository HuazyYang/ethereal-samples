#ifndef GPDEVICECUDAIMPL_H
#define GPDEVICECUDAIMPL_H
#include <gvdb/GPDevice.h>
#include <gvdb/GPDeviceCUDA.h>
#include <cuda.h>
#include <cstdarg>
#include <cassert>
#include <string>
#include <vector>
#include <map>
#include <nvrhi/core/Foundation.h>
#include <nvrhi/core/AutoPtr.h>
#include <mutex>


namespace donut::gp::cuda {

struct Device;

struct Context {
    IMessageCallback *msgCallback = nullptr;
    CUdevice cuDevice = 0;
    Device *device;
    CUcontext cuContext = 0;

    void initGlobal();
    void cuLog(CUresult rc, const char *file, int line);
    void log(MessageSeverity logLevel, const char *file, int line, const char *fmt, ...);
};

enum class GraphicsInteropAPI {
    D3D11,
    D3D12,
    Vulkan
};

struct GraphicsInteropBufferDesc {
    GraphicsInteropAPI graphicsAPI;
    void *handle;
    void *d3d11KeyedMutexHandle;
    size_t memorySize;

    size_t bufferOffset;
    size_t bufferSize;
};

struct GraphicsInteropTextureDesc {
    GraphicsInteropAPI graphicsAPI;
    void *handle;
    void *d3d11KeyedMutexHandle;
    size_t memorySize;

    uint64_t baseMipLevelMemoryOffset;
    uint32_t numMipLevels;
};

struct GraphicsInteropSemaphoreDesc {
    GraphicsInteropAPI graphicsAPI;
    void *handle;
    uint32_t initValue;
};

// T derives from IDeviceChild. A class derived from DeviceChild<T> lists T's other ancestors (e.g. IResource)
// and routes to this table.
template<typename T>
struct DeviceChild: public nvrhi::ObjectImpl<T> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(DeviceChild)
    NVRHI_IMPLEMENTS_INTERFACE(T)
    NVRHI_IMPLEMENTS_INTERFACE(IDeviceChild)
    NVRHI_END_INTERFACE_TABLE()

    IDevice *getDevice() override final {
        return m_context.device;
    }

    // Implements
    DeviceChild(Context &context) : m_context(context) {}

    Context &m_context;
};

struct Buffer: public DeviceChild<IBuffer> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Buffer)
    NVRHI_IMPLEMENTS_INTERFACE(IBuffer)
    NVRHI_IMPLEMENTS_INTERFACE(IResource)
    NVRHI_IMPLEMENTS_ROUTE_PARENT(DeviceChild<IBuffer>)
    NVRHI_END_INTERFACE_TABLE()

    const BufferDesc *getDesc() override;
    NativeHandle getNativeHandle() override;

    // Implements
    Buffer(Context &context, const BufferDesc &desc);
    Buffer(Context &context, const GraphicsInteropBufferDesc &interopDesc);
    ~Buffer();

    CUdeviceptr getDeviceAddress();

    BufferDesc m_desc;
    bool m_graphicsInterop;
    GraphicsInteropBufferDesc m_graphicsInteropDesc;

    CUdeviceptr m_cuBuffer;
    CUexternalMemory m_cuExternalMemory;
    CUexternalSemaphore m_cuD3D11KeyedMutex;
};

struct Texture: public DeviceChild<ITexture> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Texture)
    NVRHI_IMPLEMENTS_INTERFACE(ITexture)
    NVRHI_IMPLEMENTS_INTERFACE(IResource)
    NVRHI_IMPLEMENTS_ROUTE_PARENT(DeviceChild<ITexture>)
    NVRHI_END_INTERFACE_TABLE()

    const TextureDesc *getDesc() override;
    NativeHandle getMemoryHandle() override;
    NativeHandle getNativeHandle() override;
    NativeHandle getUnorderedAccessHandle(uint32_t mipIndex) override;

    // Implements
    Texture(Context &context, const TextureDesc &desc);
    Texture(Context &context, const GraphicsInteropTextureDesc &interopDesc,
            const TextureDesc &baseLayerDesc);
    ~Texture();

    TextureDesc m_desc;
    bool m_graphicsInterop;
    GraphicsInteropTextureDesc m_graphicsInteropDesc;

    union {
        CUarray m_texMemory;
        CUmipmappedArray m_texMemory2;
    };
    std::vector<CUsurfObject> m_surfaces;
    CUtexObject m_texture;
    CUexternalMemory m_cuExternalMemory;
    CUexternalSemaphore m_cuD3D11KeyedMutex;
};

struct Kernel;

struct Module: public DeviceChild<IModule> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Module)
    NVRHI_IMPLEMENTS_INTERFACE(IModule)
    NVRHI_IMPLEMENTS_ROUTE_PARENT(DeviceChild<IModule>)
    NVRHI_END_INTERFACE_TABLE()

    nvrhi::FRESULT getKernel(const char *sysName, IKernel **ppKernel) override;

    // Implements
    Module(Context &context, const ModuleDesc &desc,
        const void *data, size_t dataSize);
    ~Module();

    bool getConstant(const char *name, CUdeviceptr *dptr, size_t *bytes);

    struct ConstantInfo {
        std::string name;
        CUdeviceptr dptr;
        size_t bytes;
    };

    ModuleDesc m_desc;
    CUmodule m_cuModule;
    // Node we will place IKernel in stack
    std::map<std::string, Kernel> m_kernelLibs;
    std::vector<ConstantInfo> m_constants;
};

// Shares its Module's reference count (AddRef/Release go to the owner) but answers QueryInterface itself:
// the module does not route to its kernels. The non-delegating table answers the same interfaces.
struct Kernel: public nvrhi::DelegatingObjectImpl<IKernel> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Kernel)
    NVRHI_IMPLEMENTS_INTERFACE(IKernel)
    NVRHI_IMPLEMENTS_INTERFACE(IDeviceChild)
    NVRHI_END_INTERFACE_TABLE()
    NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE_INLINE(Kernel)
    NVRHI_IMPLEMENTS_INTERFACE(IKernel)
    NVRHI_IMPLEMENTS_INTERFACE(IDeviceChild)
    NVRHI_END_INTERFACE_TABLE()

    IDevice *getDevice() override;
    IModule *getModule() override;
    const char *getName() override;

    // Implements
    Kernel(nvrhi::IObject *pOwner, Context &context, const char *funcName, CUfunction func);
    ~Kernel();

    Context &m_context;
    const char *m_funcName;
    CUfunction m_cuFunc;
};

struct StagingBufferInfo {
    uint64_t syncPoint;
    nvrhi::AutoPtr<Buffer> buffer;
};

struct GraphicsInteropSemaphore : public DeviceChild<IGraphicsInteropSemaphore> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GraphicsInteropSemaphore)
    NVRHI_IMPLEMENTS_INTERFACE(IGraphicsInteropSemaphore)
    NVRHI_IMPLEMENTS_ROUTE_PARENT(DeviceChild<IGraphicsInteropSemaphore>)
    NVRHI_END_INTERFACE_TABLE()

    // Implements
    GraphicsInteropSemaphore(Context &context, const GraphicsInteropSemaphoreDesc &desc);
    ~GraphicsInteropSemaphore();
    GraphicsInteropSemaphoreDesc m_desc;
    CUexternalSemaphore m_cuSemaphore;
};

struct DeviceQueue: public DeviceChild<IDeviceQueue> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(DeviceQueue)
    NVRHI_IMPLEMENTS_INTERFACE(IDeviceQueue)
    NVRHI_IMPLEMENTS_ROUTE_PARENT(DeviceChild<IDeviceQueue>)
    NVRHI_END_INTERFACE_TABLE()

    nvrhi::FRESULT setConstantBuffer(IKernel *pKernel, const char *symName, void *data, size_t dataSize) override;
    nvrhi::FRESULT setConstantBuffer2(IKernel *pKernel, const char *symName, IBuffer *pBuffer,
                               uint64_t offset) override;
    nvrhi::FRESULT launch(IKernel *pKernel, const dim3 &gridDim, const dim3 &blockDim,
                   const KernelArg *args, size_t argc) override;

    nvrhi::FRESULT synchronizeQueue(IDeviceQueue *other) override;

    nvrhi::FRESULT clearBufferUint(IBuffer *buffer, uint32_t clearValue) override;
    nvrhi::FRESULT writeBuffer(IBuffer *buffer, const void *data, uint64_t bytes, uint64_t destOffsetBytes) override;
    nvrhi::FRESULT copyBufferRegion(IBuffer *dest, uint64_t destOffsetBytes, IBuffer *src,
                             uint64_t srcOffsetBytes, uint64_t dataSizeBytes) override;

    nvrhi::FRESULT copyResource(IResource *dst, IResource *src) override;
    
    nvrhi::FRESULT copyTextureRegion(const TextureCopyLocation &dst, uint32_t dstX, uint32_t dstY,
                              uint32_t dstZ, const TextureCopyLocation &src,
                              /*optional*/ const GPBox *srcBox) override;

    nvrhi::FRESULT writeTextureRegion(IResource *dstResource, uint32_t dstSubresource,
                               const void *srcData, const SubresourceFootprint &footprint,
                               uint32_t dstX, uint32_t dstY, uint32_t dstZ,
                               /* opt */ const GPBox *srcBox) override;

    nvrhi::FRESULT acquireInteropKeyedMutexes(
        const GraphicsInteropKeyedMutexWaitParams *waitParamsArray,
        uint32_t numParamsArray) override;
    nvrhi::FRESULT releaseInteropKeyedMutexes(
        const GraphicsInteropKeyedMutexSignalParams *signalParamsArray,
        uint32_t numParamsArray) override;

    nvrhi::FRESULT signalInteropSemaphore(IGraphicsInteropSemaphore *semaphore, uint64_t value) override;
    nvrhi::FRESULT waitInteropSemaphoreAsync(IGraphicsInteropSemaphore *semaphore, uint64_t value) override;

    // Implements
    DeviceQueue(Context &context, const DeviceQueueDesc &desc);
    ~DeviceQueue();

    void clearStagingResources(uint64_t syncPoint);

    DeviceQueueDesc m_desc;
    CUstream m_cuStream;
    std::vector<StagingBufferInfo> m_stagingBuffers;
    uint64_t m_lastSyncPoint;
};

struct SyncPointInfo {
    uint64_t point;
    CUevent event;
    DeviceQueue *queue;
    bool syncWithQueue;
};

struct Device : public nvrhi::ObjectImpl<IDevice> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Device)
    NVRHI_IMPLEMENTS_INTERFACE(IDevice)
    NVRHI_END_INTERFACE_TABLE()

    nvrhi::FRESULT createBuffer(const BufferDesc &desc, IBuffer **buffer) override;
    nvrhi::FRESULT createTexture(const TextureDesc &desc, ITexture **texture) override;
    nvrhi::FRESULT createModule(const ModuleDesc &desc, const void *data, size_t dataSize,
                         IModule **lib) override;
    nvrhi::FRESULT createDeviceQueue(const DeviceQueueDesc &desc, IDeviceQueue **queue) override;

    void commitQueue(IDeviceQueue *queue) override;
    nvrhi::FRESULT waitForQueue(IDeviceQueue *queue) override;
    void waitForIdle() override;

    nvrhi::FRESULT mapBuffer(IBuffer *buffer, void **data) override;
    void unmapBuffer(IBuffer *buffer) override;

    nvrhi::FRESULT createInteropD3D11Buffer(ID3D11Resource *d3d11Buffer, uint64_t mappedOffset,
                                     uint64_t mappedSize, IBuffer **buffer) override;

    nvrhi::FRESULT createInteropD3D12Buffer(ID3D12Resource *d3d12Buffer, uint64_t mappedOffset,
                                     uint64_t mappedSize, IBuffer **buffer) override;

    nvrhi::FRESULT createInteropVulkanBuffer(void *vkBuffer, void *vkMemory, void *vkDevice,
                                      uint64_t mappedOffset, uint64_t mappedSize,
                                      IBuffer **buffer) override;

    nvrhi::FRESULT createInteropD3D11Texture(ID3D11Resource *d3d11Texture,
                                      ITexture **texture) override;

    nvrhi::FRESULT createInteropD3D12Texture(ID3D12Resource *d3d12Texture,
                                      ITexture **texture) override;

    nvrhi::FRESULT createInteropVulkanTexture(void *vkImage, void *vkMemory, void *vkDevice,
                                       const TextureDesc &textureDesc,
                                       ITexture **texture) override;

    nvrhi::FRESULT createInteropD3D11Fence(ID3D11Fence *d3d11Fence,
                                    IGraphicsInteropSemaphore **semaphore) override;
    nvrhi::FRESULT createInteropD3D12Fence(ID3D12Fence *d3d12Fence,
                                    IGraphicsInteropSemaphore **semaphore) override;
    nvrhi::FRESULT createInteropVulkanSemaphore(void *vkSemaphore, void *vkDevice,
                                         IGraphicsInteropSemaphore **semaphore) override;

    // Details
    Device(const Context &context);
    ~Device();

    SyncPointInfo allocateSyncPoint(DeviceQueue *queue, bool syncWithQueue);

    Context m_context;
    std::mutex m_syncEventAllocMtx;
    std::atomic<uint64_t> m_lastSyncPoint;
    std::vector<SyncPointInfo> m_syncEvents;
    std::mutex m_syncMapResourceMtx;
};
}

#endif /* GPDEVICECUDAIMPL_H */

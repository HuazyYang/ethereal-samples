#ifndef SRC_GPDEVICE_GPDEVICE_H
#define SRC_GPDEVICE_GPDEVICE_H
#include <nvrhi/core/foundation.h>
#include <type_traits>
#include <string>

struct ID3D11Resource;
struct ID3D12Resource;
struct ID3D11Fence;
struct ID3D12Fence;
struct IDXGIKeyedMutex;

namespace donut::gp {

struct IDevice;
struct IDeviceChild;
struct IResource;
struct IBuffer;
struct ITexture;
struct IModule;
struct IKernel;

using NativeHandle = uintptr_t;

enum class MessageSeverity : uint8_t { Info, Warning, Error, Fatal };

enum class Format : uint8_t {
    UNKNOWN,

    R8_UINT,
    R8_SINT,
    R8_UNORM,
    R8_SNORM,
    RG8_UINT,
    RG8_SINT,
    RG8_UNORM,
    RG8_SNORM,
    RGBA8_UINT,
    RGBA8_SINT,
    RGBA8_UNORM,
    RGBA8_SNORM,
    BGRA8_UNORM,

    R16_UINT,
    R16_SINT,
    R16_FLOAT,
    R16_UNORM,
    R16_SNORM,
    RG16_UINT,
    RG16_SINT,
    RG16_FLOAT,
    RG16_UNORM,
    RG16_SNORM,
    RGBA16_UINT,
    RGBA16_SINT,
    RGBA16_FLOAT,
    RGBA16_UNORM,
    RGBA16_SNORM,

    R32_UINT,
    R32_SINT,
    R32_FLOAT,
    RG32_UINT,
    RG32_SINT,
    RG32_FLOAT,
    RGB32_UINT,
    RGB32_SINT,
    RGB32_FLOAT,
    RGBA32_UINT,
    RGBA32_SINT,
    RGBA32_FLOAT,

    R10G10B10A2_UNORM,

    BC1_UNORM,
    BC1_UNORM_SRGB,
    BC2_UNORM,
    BC2_UNORM_SRGB,
    BC3_UNORM,
    BC3_UNORM_SRGB,
    BC4_UNORM,
    BC4_SNORM,
    BC5_UNORM,
    BC5_SNORM,
    BC6H_UFLOAT,
    BC6H_SFLOAT,
    BC7_UNORM,
    BC7_UNORM_SRGB,

    NUM_FORMAT,
};

enum class TextureDimension {
    Texture1D,
    Texture2D,
    Texture3D
};

enum class SamplerAddressMode { Clamp, Wrap, Border, Mirror };

enum class GraphicsInteropAPI { D3D11, D3D12, VULKAN };

struct dim3 {
    int x, y, z;
};

struct Color {
    float r, g, b, a;

    Color() = default;
    Color(float c) : r(c), g(c), b(c), a(c) {}
    Color(float _r, float _g, float _b, float _a) : r(_r), g(_g), b(_b), a(_a) {}

    bool operator==(const Color &_b) const {
        return r == _b.r && g == _b.g && b == _b.b && a == _b.a;
    }
    bool operator!=(const Color &_b) const { return !(*this == _b); }
};

struct GPBox {
    uint32_t left;
    uint32_t top;
    uint32_t front;
    uint32_t right;
    uint32_t bottom;
    uint32_t back;
};

struct BufferDesc {
    size_t byteSize = 0;
    bool isStaging = false;
};

struct SamplerDesc {
    Color borderColor = 0.f;
    bool minFilter = true;  // min filter, true - linear, false - point
    bool magFilter = true;
    bool mipFilter = true;

    SamplerAddressMode addressU = SamplerAddressMode::Clamp;
    SamplerAddressMode addressV = SamplerAddressMode::Clamp;
    SamplerAddressMode addressW = SamplerAddressMode::Clamp;

    int maxAnisotropy = 0;
    float mipBias = 0.f;
    float minMipLevel = 0.f;
    float maxMipLevel = 0.f;
};

struct TextureDesc {
    TextureDimension dimension = TextureDimension::Texture2D;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depthOrArraySize = 1;
    uint32_t mipLevels = 1;

    Format format = Format::UNKNOWN;

    bool isVirtual = false;
    bool isTiled = false;

    // Sampler desc - combined texture
    SamplerDesc samplerDesc;
};

struct ModuleDesc {};

enum class KernelArgType { Scalar, Buffer, Texture_SRV, Texture_UAV };

struct KernelArg {
    KernelArgType type = KernelArgType::Scalar;

    union {
        struct {
            uint8_t data[128];
        } scalar;
        struct {
            IBuffer *buffer;
            uint64_t offset;
        } buffer;

        struct {
            ITexture *texture;
        } texture;
        struct {
            ITexture *texture;
            uint32_t mipSlice;
        } uav;
    };

    template <typename T, typename std::enable_if<sizeof(T) < 128, int>::type = 0>
    static KernelArg Scalar(const T &s);
    static KernelArg Buffer(IBuffer *pBuffer, size_t offset = 0);
    static KernelArg Texture_SRV(ITexture *pTexture);
    static KernelArg Texture_UAV(ITexture *pTexture, uint32_t mipSlice = 0);
};

template <typename T, typename std::enable_if<sizeof(T) < 128, int>::type>
inline KernelArg KernelArg::Scalar(const T &s) {
    KernelArg arg;
    arg.type = KernelArgType::Scalar;
    memcpy(arg.scalar.data, &s, sizeof(s));
    return arg;
}

inline KernelArg KernelArg::Buffer(IBuffer *pBuffer, size_t offset) {
    KernelArg arg;
    arg.type = KernelArgType::Buffer;
    arg.buffer.buffer = pBuffer;
    arg.buffer.offset = offset;
    return arg;
}

inline KernelArg KernelArg::Texture_SRV(ITexture *pTexture) {
    KernelArg arg;
    arg.type = KernelArgType::Texture_SRV;
    arg.texture.texture = pTexture;
    return arg;
}

inline KernelArg KernelArg::Texture_UAV(ITexture *pTexture, uint32_t mipSlice) {
    KernelArg arg;
    arg.type = KernelArgType::Texture_UAV;
    arg.uav.texture = pTexture;
    arg.uav.mipSlice = mipSlice;
    return arg;
}

enum DeviceQueuePriority { Highest, AboveNormal, Normal, BelowNormal, Lowest };

struct DeviceQueueDesc {
    DeviceQueuePriority priority;
};

enum class TextureCopyType { SubresourceIndex, PlacedFootprint };

inline uint32_t calcSubresourceIndex(uint32_t mipLevel, uint32_t arrayIndex, uint32_t arraySize) {
    return mipLevel * arraySize + arrayIndex;
}

struct SubresourceFootprint{
    Format format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t rowPitch;
};

struct PlacedSubresourceFootprint {
    uint64_t offset;
    Format format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t rowPitch;
};

struct TextureCopyLocation {
    IResource *resource;
    TextureCopyType type;
    union {
        PlacedSubresourceFootprint placeFootprint;
        uint32_t subresourceIndex;
    };
};

struct GraphicsInteropKeyedMutexWaitParams {
    IResource *resource;
    uint32_t key;
    /**
     * For @p timeoutMS, use ~0ull for infinite waiting and block calling thread until the
     * resource mutex is acquired
     */
    uint32_t timeoutMS;
};

struct GraphicsInteropKeyedMutexSignalParams {
    IResource *resource;
    uint32_t key;
};

NVRHI_IID(IDeviceChild, "e5d716fa-453f-46a5-af4e-51372e1fdb12")
struct IDeviceChild : nvrhi::IObject {
    NVRHI_DECLARE_UUID_TRAITS(IDeviceChild)
    virtual IDevice *getDevice() = 0;
};

NVRHI_IID(IMessageCallback, "108dbcec-9e8d-41a9-bffc-209fe1ca4f0e")
struct IMessageCallback : public nvrhi::IObject {
    NVRHI_DECLARE_UUID_TRAITS(IMessageCallback)
    virtual void message(MessageSeverity severity, const char *desc) = 0;
};

NVRHI_IID(IResource, "5ee1ef37-acc1-4c58-9b3b-8dffac082510")
struct IResource : IDeviceChild {
    NVRHI_DECLARE_UUID_TRAITS(IResource)
};

NVRHI_IID(IBuffer, "2e646679-f443-44f6-9cd8-175ffcd8a2c5")
struct IBuffer : public IResource {
    NVRHI_DECLARE_UUID_TRAITS(IBuffer)
    virtual const BufferDesc *getDesc() = 0;
    virtual NativeHandle getNativeHandle() = 0;
};

NVRHI_IID(ITexture, "7d6e963e-c33c-4d71-b919-974e6e1115d0")
struct ITexture : IResource {
    NVRHI_DECLARE_UUID_TRAITS(ITexture)
    virtual const TextureDesc *getDesc() = 0;
    virtual NativeHandle getMemoryHandle() = 0;
    virtual NativeHandle getNativeHandle() = 0;
    virtual NativeHandle getUnorderedAccessHandle(uint32_t mipIndex) = 0;
};

NVRHI_IID(IModule, "b9481856-1acf-46f6-9b9d-96aa441c8b76")
struct IModule : public IDeviceChild {
    NVRHI_DECLARE_UUID_TRAITS(IModule)
    virtual nvrhi::FRESULT getKernel(const char *sysName, IKernel **ppKernel) = 0;
};

NVRHI_IID(IKernel, "3a6fe398-bbc2-4379-9330-c231fbe2d581")
struct IKernel : public IDeviceChild {
    NVRHI_DECLARE_UUID_TRAITS(IKernel)
    virtual IModule *getModule() = 0;
    virtual const char *getName() = 0;
};

NVRHI_IID(IGraphicsInteropSemaphore, "7ea89eb4-3c02-4cd4-a1b4-f20bd5b336e1")
struct IGraphicsInteropSemaphore : IDeviceChild {
    NVRHI_DECLARE_UUID_TRAITS(IGraphicsInteropSemaphore)
};

NVRHI_IID(IGraphicsInteropKeyedMutex, "617d7adb-071b-41c4-ba88-39fb7b57dbfc")
struct IGraphicsInteropKeyedMutex : IDeviceChild {
    NVRHI_DECLARE_UUID_TRAITS(IGraphicsInteropKeyedMutex)
};

NVRHI_IID(IDeviceQueue, "73d1f44b-ad4e-44bc-b4ce-4879c1e62bac")
struct IDeviceQueue : public IDeviceChild {
    NVRHI_DECLARE_UUID_TRAITS(IDeviceQueue)
    // Backend queue object (CUDA: CUstream). For interop with libraries that need the raw stream.
    virtual NativeHandle getNativeHandle() = 0;
    virtual nvrhi::FRESULT setConstantBuffer(IKernel *pKernel, const char *symName, void *data,
                                size_t dataSize) = 0;
    virtual nvrhi::FRESULT setConstantBuffer2(IKernel *pKernel, const char *symName, IBuffer *pBuffer, uint64_t
    offset) = 0;
    virtual nvrhi::FRESULT launch(IKernel *pKernel, const dim3 &gridDim, const dim3 &blockDim,
                           const KernelArg *args, size_t argc) = 0;

    // synchronize
    virtual nvrhi::FRESULT synchronizeQueue(IDeviceQueue *otherQueue) = 0;

    // NOTE(migration): the pure-virtual signal(uint64_t)/wait(uint64_t) pair that
    // used to be declared here was never implemented or called anywhere, and only
    // made the concrete cuda:: classes abstract. Removed; use synchronizeQueue(),
    // waitForQueue()/waitForIdle(), or the interop-semaphore calls instead.

    // memory operations
    virtual nvrhi::FRESULT clearBufferUint(IBuffer *buffer, uint32_t clearValue) = 0;
    virtual nvrhi::FRESULT writeBuffer(IBuffer *buffer, const void *data, uint64_t bytes,
                                uint64_t destOffsetBytes) = 0;
    // for dataSizeInBytes which is 0, copy source buffer to its end
    virtual nvrhi::FRESULT copyBufferRegion(IBuffer *dest, uint64_t destOffsetBytes, IBuffer *src,
                                     uint64_t srcOffsetBytes, uint64_t dataSizeBytes) = 0;

    virtual nvrhi::FRESULT copyResource(IResource *src, IResource *dst) = 0;

    virtual nvrhi::FRESULT copyTextureRegion(const TextureCopyLocation &dst, uint32_t dstX,
                                      uint32_t dstY, uint32_t dstZ,
                                        const TextureCopyLocation &src, /*optional*/ const GPBox *srcBox) = 0;

    virtual nvrhi::FRESULT writeTextureRegion(IResource *dstResource, uint32_t dstSubresource, const void *srcData,
                                       const SubresourceFootprint &footprint, uint32_t dstX,
                                       uint32_t dstY, uint32_t dstZ,
                                       /* opt */ const GPBox *srcBox) = 0;

    virtual nvrhi::FRESULT acquireInteropKeyedMutexes(
        const GraphicsInteropKeyedMutexWaitParams *waitParamsArray,
        uint32_t numParamsArray) = 0;
    virtual nvrhi::FRESULT releaseInteropKeyedMutexes(
        const GraphicsInteropKeyedMutexSignalParams *signalParamsArray,
        uint32_t numParamsArray) = 0;

    virtual nvrhi::FRESULT signalInteropSemaphore(IGraphicsInteropSemaphore *semaphore, uint64_t value) = 0;
    virtual nvrhi::FRESULT waitInteropSemaphoreAsync(IGraphicsInteropSemaphore *semaphore, uint64_t value) = 0;
};

NVRHI_IID(IDevice, "f538e077-4cb4-4878-8135-6e4bd0f1ab95")
struct IDevice : nvrhi::IObject {
    NVRHI_DECLARE_UUID_TRAITS(IDevice)
    // Backend device/context object (CUDA: CUcontext).
    virtual NativeHandle getNativeHandle() = 0;
    virtual nvrhi::FRESULT createBuffer(const BufferDesc &desc, IBuffer **buffer) = 0;

    virtual nvrhi::FRESULT createTexture(const TextureDesc &desc, ITexture **texture) = 0;

    virtual nvrhi::FRESULT createModule(const ModuleDesc &desc, const void *data, size_t dataSize,
                                 IModule **lib) = 0;

    virtual nvrhi::FRESULT createDeviceQueue(const DeviceQueueDesc &desc,
                                      IDeviceQueue **queue) = 0;

    // commitQueue() records a sync point after the work submitted so far;
    // waitForQueue() blocks the host until the LAST COMMITTED sync point is
    // reached, so a host read-back needs commitQueue() first (the samples use
    // SampleUtils::syncQueue() = commit + wait). waitForIdle() drains every queue.
    virtual void commitQueue(IDeviceQueue *queue) = 0;
    virtual nvrhi::FRESULT waitForQueue(IDeviceQueue *queue) = 0;
    virtual void waitForIdle() = 0;

    // NOTE(migration): the pure-virtual signal(uint64_t)/wait(uint64_t) pair that
    // used to be declared here was never implemented or called anywhere, and only
    // made the concrete cuda:: classes abstract. Removed; use synchronizeQueue(),
    // waitForQueue()/waitForIdle(), or the interop-semaphore calls instead.

    // Only readback the buffer which has BufferDesc::isStaging set
    virtual nvrhi::FRESULT mapBuffer(IBuffer *buffer, void **data) = 0;
    virtual void unmapBuffer(IBuffer *buffer) = 0;

    // Graphics resource interoperation
    virtual nvrhi::FRESULT createInteropD3D11Buffer(ID3D11Resource *d3d11Buffer,
                                             uint64_t mappedOffset, uint64_t mappedSize,
                                             IBuffer **buffer) = 0;

    virtual nvrhi::FRESULT createInteropD3D12Buffer(ID3D12Resource *d3d12Buffer,
                                             uint64_t mappedOffset, uint64_t mappedSize,
                                             IBuffer **buffer) = 0;

    virtual nvrhi::FRESULT createInteropVulkanBuffer(void *vkBuffer, void *vkMemory, void *vkDevice, uint64_t mappedOffset,
                                              uint64_t mappedSize, IBuffer **buffer) = 0;

    /**
     * @note: for now, all texture mipmaps will be mapped for interoperation
     * so @p baseMipLevel and @p numMipLevels is reserved and should be 0
     */
    virtual nvrhi::FRESULT createInteropD3D11Texture(ID3D11Resource *d3d11Texture, ITexture **texture)
                                              = 0;

    /**
     * @note: for now, all texture mipmaps will be mapped for interoperation
     * so @p baseMipLevel and @p numMipLevels is reserved and should be 0
     */
    virtual nvrhi::FRESULT createInteropD3D12Texture(ID3D12Resource *d3d12Texture, ITexture **texture)
                                              = 0;

    virtual nvrhi::FRESULT createInteropVulkanTexture(void *vkImage, void *vkMemory, void *vkDevice,
                                               const TextureDesc &textureDesc,
                                               ITexture **texture) = 0;

    virtual nvrhi::FRESULT createInteropD3D11Fence(ID3D11Fence *d3d11Fence, IGraphicsInteropSemaphore **semaphore) = 0;
    virtual nvrhi::FRESULT createInteropD3D12Fence(ID3D12Fence *d3d12Fence,
                                            IGraphicsInteropSemaphore **semaphore) = 0;
    virtual nvrhi::FRESULT createInteropVulkanSemaphore(void *vkSemaphore, void *vkDevice,
                                                 IGraphicsInteropSemaphore **semaphore) = 0;
};

}  // namespace donut::gp

#endif /* SRC_GPDEVICE_GPDEVICE_H */

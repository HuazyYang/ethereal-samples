#include "GPDeviceCUDAImpl.h"
#include <algorithm>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11_3.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#endif
#if defined(__unix__) || defined(__linux__)
#include <unistd.h>
#endif
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

#ifdef _MSC_VER
#define GP_DEBUG_BREAK() __debugbreak()
#define GP_ALIGN(n) __declspec(align(n))
#else
#define GP_ALIGN(n) __attribute__((align(n)))
#endif

namespace donut::gp {

#define V_CUDA(context, expr)                              \
    do {                                                   \
        CUresult cuResult = (expr);                        \
        if (cuResult != CUDA_SUCCESS) {                    \
            (context).cuLog(cuResult, __FILE__, __LINE__); \
            GP_DEBUG_BREAK();                             \
        }                                                  \
    } while (0)

#define VFATAL(context, fmt, ...) \
    (context).log(MessageSeverity::Fatal, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define VERROR(context, fmt, ...) \
    (context).log(MessageSeverity::Error, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define VINFO(const, fmt, ...) \
    (context).log(MessageSeverity::Info, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define VWARNING(const, fmt, ...) \
    (context).log(MessageSeverity::Warning, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

static void vlog(IMessageCallback *pcb, MessageSeverity logLevel, const char *file,
                 int line, const char *fmt, va_list ap) {
    int n;
    const char *logRep;
    switch (logLevel) {
        case MessageSeverity::Error:
            logRep = "[Error]";
            break;
        case MessageSeverity::Fatal:
            logRep = "[Fatal]";
            break;
        case MessageSeverity::Info:
        default:
            logRep = "[Info]";
            break;
        case MessageSeverity::Warning:
            logRep = "Warning";
            break;
    }

    n = vsnprintf(nullptr, 0, fmt, ap);
    std::string s1;
    s1.resize(n + 1);
    _vsnprintf(s1.data(), s1.length(), fmt, ap);

    n = snprintf(nullptr, 0, "[donut::gp]%s %s:%d: %s\n", logRep, file, line,
                 s1.c_str());
    std::string s2;
    s2.resize(n + 1);
    snprintf(s2.data(), s2.length(), "[donut::gp]%s %s:%d: %s\n", logRep, file, line,
             s1.c_str());

    pcb->message(logLevel, s2.c_str());
}

nvrhi::FRESULT createCUDADevice(const CUDADeviceDesc &desc, IDevice **_device) {
    cuda::Context context;
    context.msgCallback = desc.messageCallback;

    context.initGlobal();

    V_CUDA(context, cuDeviceGet(&context.cuDevice, 0));

    V_CUDA(context,
           cuCtxCreate(&context.cuContext, CU_CTX_SCHED_BLOCKING_SYNC | CU_CTX_MAP_HOST,
                       context.cuDevice));

    V_CUDA(context, cuCtxSetCurrent(context.cuContext));

    auto device = MAKE_RC_OBJ(cuda::Device, context);
    if (_device) {
        *_device = device;
        device->AddRef();
    }
    device->Release();
    return nvrhi::FS_OK;
}

void memset32(void *dst, uint32_t src, size_t sz) {
    // use cache line size of 64 bytes
    GP_ALIGN(32) uint32_t line[4] = {src, src, src, src};
    sz >>= 2;
    size_t n = sz & ~3ull;
    size_t remain = sz - n;
    uint8_t *dst1 = (uint8_t *)dst;
    while(n-- > 0) {
        memcpy(dst1, line, sizeof(line));
        dst1 += sizeof(line);
    }
    memcpy(dst1, &line, remain * sizeof(uint32_t));
}

}  // namespace donut::gp

namespace donut::gp::cuda {

struct CUDAFormatInfo {
    Format format;
    CUarray_format cuFormat;
    int numComponent;
    int bytesPerBlock;
    int blockSize;
};
static CUDAFormatInfo g_CUDAFormatInfos[(int)Format::NUM_FORMAT] = {
    {Format::UNKNOWN, (CUarray_format)0, 0, 0, 0},

    {Format::R8_UINT, CU_AD_FORMAT_UNSIGNED_INT8, 1, 1, 1},
    {Format::R8_SINT, CU_AD_FORMAT_UNSIGNED_INT8, 1, 1, 1},
    {Format::R8_UNORM, CU_AD_FORMAT_UNORM_INT8X1, 1, 1, 1},
    {Format::R8_SNORM, CU_AD_FORMAT_SNORM_INT8X1, 1, 1, 1},
    {Format::RG8_UINT, CU_AD_FORMAT_UNSIGNED_INT8, 2, 2, 1},
    {Format::RG8_SINT, CU_AD_FORMAT_UNSIGNED_INT8, 2, 2, 1},
    {Format::RG8_UNORM, CU_AD_FORMAT_UNORM_INT8X2, 2, 2, 1},
    {Format::RG8_SNORM, CU_AD_FORMAT_SNORM_INT8X2, 2, 2, 1},
    {Format::RGBA8_UINT, CU_AD_FORMAT_UNSIGNED_INT8, 4, 4, 1},
    {Format::RGBA8_SINT, CU_AD_FORMAT_UNSIGNED_INT8, 4, 4, 1},
    {Format::RGBA8_UNORM, CU_AD_FORMAT_UNORM_INT8X4, 4, 4, 1},
    {Format::RGBA8_SNORM, CU_AD_FORMAT_SNORM_INT8X4, 4, 4, 1},
    {Format::BGRA8_UNORM, CU_AD_FORMAT_UNORM_INT8X4, 4, 4, 1},

    {Format::R16_UINT, CU_AD_FORMAT_UNSIGNED_INT16, 1, 2, 1},
    {Format::R16_SINT, CU_AD_FORMAT_UNSIGNED_INT16, 1, 2, 1},
    {Format::R16_FLOAT, CU_AD_FORMAT_HALF, 1, 2, 1},
    {Format::R16_UNORM, CU_AD_FORMAT_UNORM_INT16X1, 1, 2, 1},
    {Format::R16_SNORM, CU_AD_FORMAT_SNORM_INT16X1, 1, 2, 1},
    {Format::RG16_UINT, CU_AD_FORMAT_UNSIGNED_INT16, 2, 4, 1},
    {Format::RG16_SINT, CU_AD_FORMAT_UNSIGNED_INT16, 2, 4, 1},
    {Format::RG16_FLOAT, CU_AD_FORMAT_HALF, 2, 4, 1},
    {Format::RG16_UNORM, CU_AD_FORMAT_UNORM_INT16X2, 2, 4, 1},
    {Format::RG16_SNORM, CU_AD_FORMAT_SNORM_INT16X2, 2, 4, 1},
    {Format::RGBA16_UINT, CU_AD_FORMAT_UNSIGNED_INT16, 4, 8, 1},
    {Format::RGBA16_SINT, CU_AD_FORMAT_UNSIGNED_INT16, 4, 8, 1},
    {Format::RGBA16_FLOAT, CU_AD_FORMAT_HALF, 4, 8, 1},
    {Format::RGBA16_UNORM, CU_AD_FORMAT_UNORM_INT16X4, 4, 8, 1},
    {Format::RGBA16_SNORM, CU_AD_FORMAT_SNORM_INT16X4, 4, 8, 1},

    {Format::R32_UINT, CU_AD_FORMAT_UNSIGNED_INT32, 1, 4, 1},
    {Format::R32_SINT, CU_AD_FORMAT_UNSIGNED_INT32, 1, 4, 1},
    {Format::R32_FLOAT, CU_AD_FORMAT_FLOAT, 1, 4, 1},
    {Format::RG32_UINT, CU_AD_FORMAT_UNSIGNED_INT32, 2, 8, 1},
    {Format::RG32_SINT, CU_AD_FORMAT_UNSIGNED_INT32, 2, 8, 1},
    {Format::RG32_FLOAT, CU_AD_FORMAT_FLOAT, 2, 8, 1},
    {Format::RGB32_UINT, CU_AD_FORMAT_UNSIGNED_INT32, 3, 12, 1},
    {Format::RGB32_SINT, CU_AD_FORMAT_SIGNED_INT32, 3, 12, 1},
    {Format::RGB32_FLOAT, CU_AD_FORMAT_FLOAT, 3, 12, 1},
    {Format::RGBA32_UINT, CU_AD_FORMAT_UNSIGNED_INT32, 4, 16, 1},
    {Format::RGBA32_SINT, CU_AD_FORMAT_UNSIGNED_INT32, 4, 16, 1},
    {Format::RGBA32_FLOAT, CU_AD_FORMAT_FLOAT, 4, 16, 1},

    {Format::R10G10B10A2_UNORM, CU_AD_FORMAT_UNORM_INT_101010_2, 4, 4, 1},

    {Format::BC1_UNORM, CU_AD_FORMAT_BC1_UNORM, 1, 8, 4},
    {Format::BC1_UNORM_SRGB, CU_AD_FORMAT_BC1_UNORM_SRGB, 1, 8, 4},
    {Format::BC2_UNORM, CU_AD_FORMAT_BC2_UNORM, 1, 16, 4},
    {Format::BC2_UNORM_SRGB, CU_AD_FORMAT_BC2_UNORM_SRGB, 1, 16, 4},
    {Format::BC3_UNORM, CU_AD_FORMAT_BC3_UNORM, 1, 16, 4},
    {Format::BC3_UNORM_SRGB, CU_AD_FORMAT_BC3_UNORM_SRGB, 1, 16, 4},
    {Format::BC4_UNORM, CU_AD_FORMAT_BC4_UNORM, 1, 8, 4},
    {Format::BC4_SNORM, CU_AD_FORMAT_BC4_SNORM, 1, 8, 4},
    {Format::BC5_UNORM, CU_AD_FORMAT_BC5_UNORM, 1, 16, 4},
    {Format::BC5_SNORM, CU_AD_FORMAT_BC5_SNORM, 1, 16, 4},
    {Format::BC6H_UFLOAT, CU_AD_FORMAT_BC6H_UF16, 1, 16, 4},
    {Format::BC6H_SFLOAT, CU_AD_FORMAT_BC6H_SF16, 1, 16, 4},
    {Format::BC7_UNORM, CU_AD_FORMAT_BC7_UNORM, 1, 16, 4},
    {Format::BC7_UNORM_SRGB, CU_AD_FORMAT_BC7_UNORM_SRGB, 1, 16, 4},
};

// Format mapping table. The rows must be in the exactly same order as Format enum members
// are defined.
struct DxgiFormatMapping {
    Format format;
    DXGI_FORMAT resFormat;
    DXGI_FORMAT srvFormat;
    DXGI_FORMAT rtvFormat;
};
static const DxgiFormatMapping c_FormatMappings[] = {
    {Format::UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN},

    {Format::R8_UINT, DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8_UINT},
    {Format::R8_SINT, DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_SINT, DXGI_FORMAT_R8_SINT},
    {Format::R8_UNORM, DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM},
    {Format::R8_SNORM, DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8_SNORM},
    {Format::RG8_UINT, DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UINT,
     DXGI_FORMAT_R8G8_UINT},
    {Format::RG8_SINT, DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_SINT,
     DXGI_FORMAT_R8G8_SINT},
    {Format::RG8_UNORM, DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM,
     DXGI_FORMAT_R8G8_UNORM},
    {Format::RG8_SNORM, DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_SNORM,
     DXGI_FORMAT_R8G8_SNORM},
    {Format::RGBA8_UINT, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UINT,
     DXGI_FORMAT_R8G8B8A8_UINT},
    {Format::RGBA8_SINT, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_SINT,
     DXGI_FORMAT_R8G8B8A8_SINT},
    {Format::RGBA8_UNORM, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
     DXGI_FORMAT_R8G8B8A8_UNORM},
    {Format::RGBA8_SNORM, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_SNORM,
     DXGI_FORMAT_R8G8B8A8_SNORM},
    {Format::BGRA8_UNORM, DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM,
     DXGI_FORMAT_B8G8R8A8_UNORM},

    {Format::R16_UINT, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UINT,
     DXGI_FORMAT_R16_UINT},
    {Format::R16_SINT, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_SINT,
     DXGI_FORMAT_R16_SINT},
    {Format::R16_FLOAT, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_FLOAT,
     DXGI_FORMAT_R16_FLOAT},
    {Format::R16_UNORM, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM,
     DXGI_FORMAT_R16_UNORM},
    {Format::R16_SNORM, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_SNORM,
     DXGI_FORMAT_R16_SNORM},
    {Format::RG16_UINT, DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UINT,
     DXGI_FORMAT_R16G16_UINT},
    {Format::RG16_SINT, DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_SINT,
     DXGI_FORMAT_R16G16_SINT},
    {Format::RG16_FLOAT, DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT,
     DXGI_FORMAT_R16G16_FLOAT},
    {Format::RG16_UNORM, DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UNORM,
     DXGI_FORMAT_R16G16_UNORM},
    {Format::RG16_SNORM, DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_SNORM,
     DXGI_FORMAT_R16G16_SNORM},
    {Format::RGBA16_UINT, DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UINT,
     DXGI_FORMAT_R16G16B16A16_UINT},
    {Format::RGBA16_SINT, DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_SINT,
     DXGI_FORMAT_R16G16B16A16_SINT},
    {Format::RGBA16_FLOAT, DXGI_FORMAT_R16G16B16A16_TYPELESS,
     DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT},
    {Format::RGBA16_UNORM, DXGI_FORMAT_R16G16B16A16_TYPELESS,
     DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM},
    {Format::RGBA16_SNORM, DXGI_FORMAT_R16G16B16A16_TYPELESS,
     DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM},

    {Format::R32_UINT, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_UINT,
     DXGI_FORMAT_R32_UINT},
    {Format::R32_SINT, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_SINT,
     DXGI_FORMAT_R32_SINT},
    {Format::R32_FLOAT, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT,
     DXGI_FORMAT_R32_FLOAT},
    {Format::RG32_UINT, DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_UINT,
     DXGI_FORMAT_R32G32_UINT},
    {Format::RG32_SINT, DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_SINT,
     DXGI_FORMAT_R32G32_SINT},
    {Format::RG32_FLOAT, DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_FLOAT,
     DXGI_FORMAT_R32G32_FLOAT},
    {Format::RGB32_UINT, DXGI_FORMAT_R32G32B32_TYPELESS, DXGI_FORMAT_R32G32B32_UINT,
     DXGI_FORMAT_R32G32B32_UINT},
    {Format::RGB32_SINT, DXGI_FORMAT_R32G32B32_TYPELESS, DXGI_FORMAT_R32G32B32_SINT,
     DXGI_FORMAT_R32G32B32_SINT},
    {Format::RGB32_FLOAT, DXGI_FORMAT_R32G32B32_TYPELESS, DXGI_FORMAT_R32G32B32_FLOAT,
     DXGI_FORMAT_R32G32B32_FLOAT},
    {Format::RGBA32_UINT, DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_UINT,
     DXGI_FORMAT_R32G32B32A32_UINT},
    {Format::RGBA32_SINT, DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_SINT,
     DXGI_FORMAT_R32G32B32A32_SINT},
    {Format::RGBA32_FLOAT, DXGI_FORMAT_R32G32B32A32_TYPELESS,
     DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT},

    {Format::R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_TYPELESS,
     DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM},

    {Format::BC1_UNORM, DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM,
     DXGI_FORMAT_BC1_UNORM},
    {Format::BC1_UNORM_SRGB, DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM_SRGB,
     DXGI_FORMAT_BC1_UNORM_SRGB},
    {Format::BC2_UNORM, DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM,
     DXGI_FORMAT_BC2_UNORM},
    {Format::BC2_UNORM_SRGB, DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM_SRGB,
     DXGI_FORMAT_BC2_UNORM_SRGB},
    {Format::BC3_UNORM, DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM,
     DXGI_FORMAT_BC3_UNORM},
    {Format::BC3_UNORM_SRGB, DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM_SRGB,
     DXGI_FORMAT_BC3_UNORM_SRGB},
    {Format::BC4_UNORM, DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_UNORM,
     DXGI_FORMAT_BC4_UNORM},
    {Format::BC4_SNORM, DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_SNORM,
     DXGI_FORMAT_BC4_SNORM},
    {Format::BC5_UNORM, DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_UNORM,
     DXGI_FORMAT_BC5_UNORM},
    {Format::BC5_SNORM, DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_SNORM,
     DXGI_FORMAT_BC5_SNORM},
    {Format::BC6H_UFLOAT, DXGI_FORMAT_BC6H_TYPELESS, DXGI_FORMAT_BC6H_UF16,
     DXGI_FORMAT_BC6H_UF16},
    {Format::BC6H_SFLOAT, DXGI_FORMAT_BC6H_TYPELESS, DXGI_FORMAT_BC6H_SF16,
     DXGI_FORMAT_BC6H_SF16},
    {Format::BC7_UNORM, DXGI_FORMAT_BC7_TYPELESS, DXGI_FORMAT_BC7_UNORM,
     DXGI_FORMAT_BC7_UNORM},
    {Format::BC7_UNORM_SRGB, DXGI_FORMAT_BC7_TYPELESS, DXGI_FORMAT_BC7_UNORM_SRGB,
     DXGI_FORMAT_BC7_UNORM_SRGB},
};

static bool getFormatFromDXGIFormat(DXGI_FORMAT dxgiFormat, Format *format) {
    for (auto &mapping : c_FormatMappings) {
        if (mapping.resFormat == dxgiFormat || mapping.srvFormat == dxgiFormat) {
            *format = mapping.format;
            return true;
        }
    }
    return false;
}

static const CUDAFormatInfo &getFormatInfo(Format format) {
    int idx = int(format);
    if (idx >= int(Format::NUM_FORMAT)) return g_CUDAFormatInfos[0];

    auto &info = g_CUDAFormatInfos[idx];
    NVRHI_ASSERT(info.format == format);

    return info;
}

static const CUaddress_mode getSamplerAddressMode(SamplerAddressMode mode) {
    switch (mode) {
        case SamplerAddressMode::Clamp:
            return CU_TR_ADDRESS_MODE_CLAMP;
        case SamplerAddressMode::Wrap:
            return CU_TR_ADDRESS_MODE_WRAP;
        case SamplerAddressMode::Border:
            return CU_TR_ADDRESS_MODE_BORDER;
        case SamplerAddressMode::Mirror:
            return CU_TR_ADDRESS_MODE_MIRROR;
        default:
            NVRHI_ASSERT(0);
    }
    return CU_TR_ADDRESS_MODE_CLAMP;
}

template <typename T1, typename T2>
T1 *checked_cast(T2 *ptr) {
    return static_cast<T1 *>(ptr);
}

void Context::initGlobal() { V_CUDA((*this), cuInit(0)); }

void Context::cuLog(CUresult rc, const char *file, int line) {
    const char *errDesc, *errName;
    cuGetErrorString(rc, &errDesc);
    cuGetErrorName(rc, &errName);
    log(MessageSeverity::Error, file, line, "[CUDA] %s(%s)", errDesc, errName);
}

void Context::log(MessageSeverity logLevel, const char *file, int line, const char *fmt,
                  ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(msgCallback, logLevel, file, line, fmt, ap);
    va_end(ap);
}

NativeHandle Buffer::getNativeHandle() { return (NativeHandle)m_cuBuffer; }

Buffer::Buffer(Context &context, const BufferDesc &desc)
    : DeviceChild<IBuffer>(context),
      m_desc(desc),
      m_graphicsInterop(false),
      m_graphicsInteropDesc{},
      m_cuBuffer{},
      m_cuExternalMemory{},
      m_cuD3D11KeyedMutex{} {
    if (m_desc.byteSize != 0) {
        if (m_desc.isStaging)
            V_CUDA(context, cuMemAllocHost((void **)&m_cuBuffer, m_desc.byteSize));
        else
            V_CUDA(context, cuMemAlloc(&m_cuBuffer, m_desc.byteSize));
    }
}

Buffer::Buffer(Context &context, const GraphicsInteropBufferDesc &interopDesc)
    : DeviceChild<IBuffer>(context),
      m_desc{},
      m_graphicsInterop(true),
      m_graphicsInteropDesc{interopDesc},
      m_cuBuffer{},
      m_cuExternalMemory{},
      m_cuD3D11KeyedMutex{} {
    // memory
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC memDesc = {};
    switch (interopDesc.graphicsAPI) {
        case GraphicsInteropAPI::D3D11:
            memDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_RESOURCE;
            break;
        case GraphicsInteropAPI::D3D12:
            memDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE;
            break;
        case GraphicsInteropAPI::Vulkan:
#ifdef _WIN32
            memDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
#else
            memDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
#endif
            break;
    }
    memDesc.handle.win32.handle = m_graphicsInteropDesc.handle;
    memDesc.size = m_graphicsInteropDesc.memorySize;
    memDesc.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
    V_CUDA(context, cuImportExternalMemory(&m_cuExternalMemory, &memDesc));

    // buffer
    CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufDesc = {};
    bufDesc.offset = m_graphicsInteropDesc.bufferOffset;
    bufDesc.size = m_graphicsInteropDesc.bufferSize;
    V_CUDA(context,
           cuExternalMemoryGetMappedBuffer(&m_cuBuffer, m_cuExternalMemory, &bufDesc));

   if(interopDesc.graphicsAPI == GraphicsInteropAPI::D3D11) {
       CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC semDesc = {};
       semDesc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_KEYED_MUTEX;
       semDesc.handle.win32.handle = m_graphicsInteropDesc.d3d11KeyedMutexHandle;
       V_CUDA(context, cuImportExternalSemaphore(&m_cuD3D11KeyedMutex, &semDesc));
   }

    m_desc.byteSize = bufDesc.size;
}

Buffer::~Buffer() {
    if (m_cuBuffer) {
        if (m_desc.isStaging)
            V_CUDA(m_context, cuMemFreeHost((void *)m_cuBuffer));
        else
            V_CUDA(m_context, cuMemFree(m_cuBuffer));
        m_cuBuffer = 0;
    }

    if (m_graphicsInterop) {
        V_CUDA(m_context, cuDestroyExternalMemory(m_cuExternalMemory));
        m_cuExternalMemory = 0;

#ifdef _WIN32
        // Close handle here
        CloseHandle((HANDLE)m_graphicsInteropDesc.handle);
#else
        close((int)m_graphicsInteropDesc.handle);
#endif
        m_graphicsInteropDesc.handle = nullptr;
#ifdef _WIN32
        // if(m_graphicsInteropDesc.d3d11KeyedMutexHandle) {
        //     CloseHandle((HANDLE)m_graphicsInteropDesc.d3d11KeyedMutexHandle);
        //     m_graphicsInteropDesc.d3d11KeyedMutexHandle = nullptr;
        // }
        if (m_cuD3D11KeyedMutex) {
            V_CUDA(m_context, cuDestroyExternalSemaphore(m_cuD3D11KeyedMutex));
            m_cuD3D11KeyedMutex = 0;
        }
#endif
    }
}

const TextureDesc *Texture::getDesc() { return &m_desc; }

NativeHandle Texture::getMemoryHandle() { return (NativeHandle)m_texMemory; }

NativeHandle Texture::getNativeHandle() { return (NativeHandle)m_texture; }

NativeHandle Texture::getUnorderedAccessHandle(uint32_t mipIndex) {
    if (mipIndex >= m_surfaces.size()) return 0;

    return (NativeHandle)m_surfaces[mipIndex];
}

Texture::Texture(Context &context, const TextureDesc &desc)
    : DeviceChild<ITexture>(context),
      m_desc{desc},
      m_graphicsInterop{false},
      m_graphicsInteropDesc{},
      m_texMemory{},
      m_surfaces{},
      m_texture{},
      m_cuExternalMemory{},
      m_cuD3D11KeyedMutex{} {
    m_desc.mipLevels = std::max(1u, m_desc.mipLevels);

    CUDA_ARRAY3D_DESCRIPTOR arrayDesc;
    auto &formatInfo = getFormatInfo(m_desc.format);
    arrayDesc.Format = formatInfo.cuFormat;
    arrayDesc.NumChannels = formatInfo.numComponent;
    arrayDesc.Flags = CUDA_ARRAY3D_SURFACE_LDST;

    switch (m_desc.dimension) {
        case TextureDimension::Texture1D: {
            arrayDesc.Width = m_desc.width;
            arrayDesc.Height = 0;
            if (m_desc.depthOrArraySize == 1) {
                arrayDesc.Depth = 0;
            } else {
                arrayDesc.Depth = m_desc.depthOrArraySize;
                arrayDesc.Flags |= CUDA_ARRAY3D_LAYERED;
            }
            break;
        }
        case TextureDimension::Texture2D: {
            arrayDesc.Width = m_desc.width;
            arrayDesc.Height = m_desc.height;
            if (m_desc.depthOrArraySize == 1) {
                arrayDesc.Depth = 0;
            } else {
                arrayDesc.Depth = m_desc.depthOrArraySize;
                arrayDesc.Flags |= CUDA_ARRAY3D_LAYERED;
            }
            break;
        }
        case TextureDimension::Texture3D: {
            arrayDesc.Width = m_desc.width;
            arrayDesc.Height = m_desc.height;
            arrayDesc.Depth = m_desc.depthOrArraySize;
        }
        break;
        default:
            NVRHI_ASSERT(0);
    }

    CUDA_RESOURCE_DESC resDesc = {};
    resDesc.resType = CU_RESOURCE_TYPE_ARRAY;

    if (m_desc.mipLevels == 1) {
        V_CUDA(m_context, cuArray3DCreate(&m_texMemory, &arrayDesc));
        m_surfaces.resize(1);

        resDesc.res.array.hArray = m_texMemory;

        V_CUDA(m_context, cuSurfObjectCreate(&m_surfaces[0], &resDesc));
    } else {
        V_CUDA(m_context,
               cuMipmappedArrayCreate(&m_texMemory2, &arrayDesc, m_desc.mipLevels));
        m_surfaces.resize(m_desc.mipLevels);

        CUarray mipSlice;
        for (int i = 0; i < m_desc.mipLevels; ++i) {
            V_CUDA(m_context, cuMipmappedArrayGetLevel(&mipSlice, m_texMemory2, i));
            resDesc.res.array.hArray = mipSlice;
            V_CUDA(m_context, cuSurfObjectCreate(&m_surfaces[i], &resDesc));
        }
    }

    auto &samplerDesc = m_desc.samplerDesc;
    CUDA_TEXTURE_DESC texDesc = {};
    texDesc.addressMode[0] = getSamplerAddressMode(samplerDesc.addressU);
    texDesc.addressMode[1] = getSamplerAddressMode(samplerDesc.addressV);
    texDesc.addressMode[2] = getSamplerAddressMode(samplerDesc.addressW);
    texDesc.filterMode =
        samplerDesc.minFilter ? CU_TR_FILTER_MODE_LINEAR : CU_TR_FILTER_MODE_POINT;
    texDesc.mipmapFilterMode =
        samplerDesc.minFilter ? CU_TR_FILTER_MODE_LINEAR : CU_TR_FILTER_MODE_POINT;
    texDesc.maxAnisotropy = samplerDesc.maxAnisotropy;
    texDesc.mipmapLevelBias = samplerDesc.mipBias;
    texDesc.minMipmapLevelClamp = samplerDesc.minMipLevel;
    texDesc.maxMipmapLevelClamp = samplerDesc.maxMipLevel;
    memcpy(texDesc.borderColor, &samplerDesc.borderColor, sizeof(texDesc.borderColor));

    CUDA_RESOURCE_DESC texResDesc = {};
    if (m_desc.mipLevels == 1) {
        texResDesc.resType = CU_RESOURCE_TYPE_ARRAY;
        texResDesc.res.array.hArray = m_texMemory;
        V_CUDA(m_context, cuTexObjectCreate(&m_texture, &texResDesc, &texDesc, nullptr));
    } else {
        texResDesc.resType = CU_RESOURCE_TYPE_MIPMAPPED_ARRAY;
        texResDesc.res.mipmap.hMipmappedArray = m_texMemory2;
        V_CUDA(m_context, cuTexObjectCreate(&m_texture, &texResDesc, &texDesc, nullptr));
    }
}

Texture::Texture(Context &context, const GraphicsInteropTextureDesc &interopDesc,
                 const TextureDesc &baseLayerDesc)
    : DeviceChild<ITexture>{context},
      m_desc{baseLayerDesc},
      m_graphicsInterop{true},
      m_graphicsInteropDesc{interopDesc},
      m_texMemory2{},
      m_surfaces{},
      m_texture{},
      m_cuExternalMemory{},
      m_cuD3D11KeyedMutex{} {
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC memDesc = {};
    switch (interopDesc.graphicsAPI) {
        case GraphicsInteropAPI::D3D11:
            memDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_RESOURCE;
            break;
        case GraphicsInteropAPI::D3D12:
            memDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE;
            break;
        case GraphicsInteropAPI::Vulkan:
#ifdef _WIN32
            memDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
#else
            memDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
#endif
            break;
    }
    memDesc.handle.win32.handle = m_graphicsInteropDesc.handle;
    memDesc.size = m_graphicsInteropDesc.memorySize;
    memDesc.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
    V_CUDA(context, cuImportExternalMemory(&m_cuExternalMemory, &memDesc));

    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC mipmapDesc = {};
    mipmapDesc.offset = m_graphicsInteropDesc.baseMipLevelMemoryOffset;
    mipmapDesc.numLevels = m_graphicsInteropDesc.numMipLevels;

    CUDA_ARRAY3D_DESCRIPTOR arrayDesc = {};
    auto &formatInfo = getFormatInfo(m_desc.format);
    arrayDesc.Format = formatInfo.cuFormat;
    arrayDesc.NumChannels = formatInfo.numComponent;
    arrayDesc.Flags = CUDA_ARRAY3D_SURFACE_LDST;

    switch (m_desc.dimension) {
        case TextureDimension::Texture1D: {
            arrayDesc.Width = m_desc.width;
            arrayDesc.Height = 0;
            arrayDesc.Depth = m_desc.depthOrArraySize;
            arrayDesc.Flags |= CUDA_ARRAY3D_LAYERED;
            break;
        }
        case TextureDimension::Texture2D: {
            arrayDesc.Width = m_desc.width;
            arrayDesc.Height = m_desc.height;
            arrayDesc.Depth = m_desc.depthOrArraySize;
            arrayDesc.Flags |= CUDA_ARRAY3D_LAYERED;
            break;
        }
        case TextureDimension::Texture3D: {
            arrayDesc.Width = m_desc.width;
            arrayDesc.Height = m_desc.height;
            arrayDesc.Depth = m_desc.depthOrArraySize;
        }
        default:
            NVRHI_ASSERT(0);
    }

    mipmapDesc.arrayDesc = arrayDesc;

    V_CUDA(m_context, cuExternalMemoryGetMappedMipmappedArray(
                          &m_texMemory2, m_cuExternalMemory, &mipmapDesc));

    {
        m_desc.mipLevels = m_graphicsInteropDesc.numMipLevels;
        m_surfaces.resize(m_desc.mipLevels);

        CUDA_RESOURCE_DESC resDesc = {};
        resDesc.resType = CU_RESOURCE_TYPE_ARRAY;
        CUarray mipSlice;
        for (int i = 0; i < m_desc.mipLevels; ++i) {
            V_CUDA(m_context, cuMipmappedArrayGetLevel(&mipSlice, m_texMemory2, i));
            resDesc.res.array.hArray = mipSlice;
            V_CUDA(m_context, cuSurfObjectCreate(&m_surfaces[i], &resDesc));
        }

        auto &samplerDesc = m_desc.samplerDesc;
        CUDA_TEXTURE_DESC texDesc = {};
        texDesc.addressMode[0] = getSamplerAddressMode(samplerDesc.addressU);
        texDesc.addressMode[1] = getSamplerAddressMode(samplerDesc.addressV);
        texDesc.addressMode[2] = getSamplerAddressMode(samplerDesc.addressW);
        texDesc.filterMode =
            samplerDesc.minFilter ? CU_TR_FILTER_MODE_LINEAR : CU_TR_FILTER_MODE_POINT;
        texDesc.mipmapFilterMode =
            samplerDesc.minFilter ? CU_TR_FILTER_MODE_LINEAR : CU_TR_FILTER_MODE_POINT;
        texDesc.maxAnisotropy = samplerDesc.maxAnisotropy;
        texDesc.mipmapLevelBias = samplerDesc.mipBias;
        texDesc.minMipmapLevelClamp = samplerDesc.minMipLevel;
        texDesc.maxMipmapLevelClamp = samplerDesc.maxMipLevel;
        memcpy(texDesc.borderColor, &samplerDesc.borderColor, sizeof(texDesc.borderColor));

        CUDA_RESOURCE_DESC texResDesc = {};
        texResDesc.resType = CU_RESOURCE_TYPE_MIPMAPPED_ARRAY;
        texResDesc.res.mipmap.hMipmappedArray = m_texMemory2;
        V_CUDA(m_context, cuTexObjectCreate(&m_texture, &texResDesc, &texDesc, nullptr));
    }

    if (interopDesc.graphicsAPI == GraphicsInteropAPI::D3D11) {
        CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC semDesc = {};
        semDesc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_KEYED_MUTEX;
        semDesc.handle.win32.handle = m_graphicsInteropDesc.d3d11KeyedMutexHandle;
        V_CUDA(context, cuImportExternalSemaphore(&m_cuD3D11KeyedMutex, &semDesc));
    }
}

Texture::~Texture() {
    if (m_texture) {
        V_CUDA(m_context, cuTexObjectDestroy(m_texture));
        m_texture = 0;
    }

    for (auto &surf : m_surfaces) {
        if (surf) {
            V_CUDA(m_context, cuSurfObjectDestroy(surf));
            surf = 0;
        }
    }
    if (m_texMemory) {
        if (!m_graphicsInterop) {
            if (m_desc.mipLevels == 1)
                V_CUDA(m_context, cuArrayDestroy(m_texMemory));
            else
                V_CUDA(m_context, cuMipmappedArrayDestroy(m_texMemory2));
        } else
            V_CUDA(m_context, cuMipmappedArrayDestroy(m_texMemory2));
        m_texMemory = 0;
    }

    if (m_graphicsInterop) {
        V_CUDA(m_context, cuDestroyExternalMemory(m_cuExternalMemory));
        m_cuExternalMemory = 0;
        // Close handle here
#ifdef _WIN32
        CloseHandle((HANDLE)m_graphicsInteropDesc.handle);
#else
        close((int)m_graphicsInteropDesc.handle)
#endif
        m_graphicsInteropDesc.handle = nullptr;

#ifdef _WIN32
        // if (m_graphicsInteropDesc.d3d11KeyedMutexHandle) {
        //     CloseHandle((HANDLE)m_graphicsInteropDesc.d3d11KeyedMutexHandle);
        //     m_graphicsInteropDesc.d3d11KeyedMutexHandle = nullptr;
        // }
        if (m_cuD3D11KeyedMutex) {
            V_CUDA(m_context, cuDestroyExternalSemaphore(m_cuD3D11KeyedMutex));
            m_cuD3D11KeyedMutex = 0;
        }
#endif

    }
}

CUdeviceptr Buffer::getDeviceAddress() {
    if (m_desc.isStaging) {
        CUdeviceptr dptr;
        V_CUDA(m_context, cuMemHostGetDevicePointer(&dptr, (void *)m_cuBuffer, 0));
        return dptr;
    } else
        return m_cuBuffer;
}

const BufferDesc *Buffer::getDesc() { return &m_desc; }


nvrhi::FRESULT DeviceQueue::launch(IKernel *pKernel, const dim3 &gridDim, const dim3 &blockDim,
                            const KernelArg *args, size_t argc) {
    if (pKernel == nullptr) return nvrhi::FE_INVALID_ARGS;

    int auxPtrCnt = 0;
    for (size_t i = 0; i < argc; ++i) {
        if (args[i].type == KernelArgType::Buffer) ++auxPtrCnt;
    }

    std::vector<void *> cuArgs(argc +
                               auxPtrCnt);  // Append sufficient space for buffer type
    size_t nextBuffIdx = argc;
    for (size_t i = 0; i < argc; ++i) {
        auto &arg = args[i];
        switch (arg.type) {
            case KernelArgType::Scalar:
                cuArgs[i] = (void *)arg.scalar.data;
                break;
            case KernelArgType::Buffer: {
                auto pBuffer = checked_cast<Buffer>(arg.buffer.buffer);
                if (pBuffer) {
                    cuArgs[nextBuffIdx] =
                        (uint8_t *)pBuffer->m_cuBuffer + arg.buffer.offset;
                    cuArgs[i] = &cuArgs[nextBuffIdx++];
                } else
                    cuArgs[i] = nullptr;
                break;
            }
            case KernelArgType::Texture_SRV: {
                auto pTexture = checked_cast<Texture>(arg.texture.texture);
                if (pTexture) {
                    cuArgs[i] = &pTexture->m_texture;
                } else
                    cuArgs[i] = nullptr;
                break;
            }
            case KernelArgType::Texture_UAV: {
                auto pTexture = checked_cast<Texture>(arg.uav.texture);
                if (pTexture) {
                    NVRHI_ASSERT(arg.uav.mipSlice < pTexture->m_desc.mipLevels);
                    cuArgs[i] = &pTexture->m_surfaces[arg.uav.mipSlice];
                } else
                    cuArgs[i] = nullptr;
                break;
            }
            default:
                cuArgs[i] = nullptr;
        }
    }

    auto pKernelCUDA = checked_cast<Kernel>(pKernel);
    CUresult cures =
        cuLaunchKernel(pKernelCUDA->m_cuFunc, gridDim.x, gridDim.y, gridDim.z, blockDim.x,
                       blockDim.y, blockDim.z, 0, m_cuStream, &cuArgs[0], nullptr);
    V_CUDA(m_context, cures);
    if (cures != CUDA_SUCCESS) return nvrhi::FE_GENERIC_ERROR;

    return nvrhi::FS_OK;
}

nvrhi::FRESULT DeviceQueue::synchronizeQueue(IDeviceQueue *_other) {
    auto otherQueue = checked_cast<DeviceQueue>(_other);
    if (otherQueue == nullptr) return nvrhi::FE_INVALID_ARGS;

    auto syncPoint = m_context.device->allocateSyncPoint(this, true);
    V_CUDA(m_context, cuEventRecord(syncPoint.event, otherQueue->m_cuStream));
    m_lastSyncPoint = syncPoint.point;
    V_CUDA(m_context,
           cuStreamWaitEvent(m_cuStream, syncPoint.event, CU_EVENT_WAIT_DEFAULT));
    return nvrhi::FS_OK;
}

nvrhi::FRESULT DeviceQueue::clearBufferUint(IBuffer *_buffer, uint32_t clearValue) {
    Buffer *buffer = checked_cast<Buffer>(_buffer);
    if (buffer == nullptr || buffer->m_desc.byteSize == 0 ||
        (buffer->m_desc.byteSize & 3) != 0)
        return nvrhi::FE_INVALID_ARGS;

    if (buffer->m_desc.isStaging) {

        memset32((uint8_t *)buffer->m_cuBuffer, clearValue, buffer->m_desc.byteSize);
    } else {
        V_CUDA(m_context, cuMemsetD32Async(buffer->m_cuBuffer, clearValue,
                                           buffer->m_desc.byteSize >> 2, m_cuStream));
    }
    return nvrhi::FS_OK;
}

nvrhi::FRESULT DeviceQueue::writeBuffer(IBuffer *_buffer, const void *data, uint64_t bytes,
                                 uint64_t destOffsetBytes) {
    Buffer *buffer = checked_cast<Buffer>(_buffer);
    if (buffer == nullptr || buffer->m_desc.byteSize == 0 || data == nullptr ||
        bytes == 0 || bytes + destOffsetBytes > buffer->m_desc.byteSize)
        return nvrhi::FE_INVALID_ARGS;

    if (buffer->m_desc.isStaging)
        memcpy((uint8_t *)buffer->m_cuBuffer + destOffsetBytes, data, bytes);
    else {
        BufferDesc bufDesc;
        bufDesc.isStaging = true;
        bufDesc.byteSize = bytes;
        auto stagingBuffer = MAKE_RC_OBJ(Buffer, m_context, bufDesc);
        memcpy((void *)stagingBuffer->m_cuBuffer, data, bytes);
        m_stagingBuffers.push_back({m_lastSyncPoint, nvrhi::TakeOver(stagingBuffer)});

        CUdeviceptr dstPtr = (CUdeviceptr)((uint8_t *)buffer->m_cuBuffer + destOffsetBytes);
        V_CUDA(m_context, cuMemcpyHtoDAsync(dstPtr, (void *)stagingBuffer->m_cuBuffer,
                                            bytes, m_cuStream));
    }
    return nvrhi::FS_OK;
}

nvrhi::FRESULT DeviceQueue::copyBufferRegion(IBuffer *_dest, uint64_t destOffsetBytes,
                                      IBuffer *_src, uint64_t srcOffsetBytes,
                                      uint64_t dataSizeBytes) {
    auto dest = checked_cast<Buffer>(_dest);
    auto src = checked_cast<Buffer>(_src);

    if (dataSizeBytes == 0 && srcOffsetBytes < src->m_desc.byteSize)
        dataSizeBytes = src->m_desc.byteSize - srcOffsetBytes;

    if (dest == nullptr || src == nullptr || dest->m_desc.byteSize == 0 ||
        src->m_desc.byteSize == 0 ||
        destOffsetBytes + dataSizeBytes > dest->m_desc.byteSize ||
        srcOffsetBytes + dataSizeBytes > src->m_desc.byteSize)
        return nvrhi::FE_INVALID_ARGS;

    void *destPtr = (uint8_t *)dest->m_cuBuffer + destOffsetBytes;
    void *srcPtr = (uint8_t *)src->m_cuBuffer + srcOffsetBytes;

    if (dest->m_desc.isStaging) {
        if (src->m_desc.isStaging)
            memcpy(destPtr, srcPtr, dataSizeBytes);
        else {
            V_CUDA(m_context, cuMemcpyAsync((CUdeviceptr)destPtr, (CUdeviceptr)srcPtr, dataSizeBytes,
                                                m_cuStream));
        }
    } else {
        if (src->m_desc.isStaging) {
            V_CUDA(m_context,
                   cuMemcpyAsync((CUdeviceptr)destPtr, (CUdeviceptr)srcPtr, dataSizeBytes, m_cuStream));
        } else {
            V_CUDA(m_context, cuMemcpyAsync((CUdeviceptr)destPtr, (CUdeviceptr)srcPtr,
                                            dataSizeBytes, m_cuStream));
        }
    }
    return nvrhi::FS_OK;
}

nvrhi::FRESULT DeviceQueue::copyResource(IResource *dst, IResource *src) {
    if (!dst || !src) return nvrhi::FE_INVALID_ARGS;
    {
        nvrhi::AutoPtr<IBuffer> srcBuffer, dstBuffer;

        if (NVRHI_SUCCEEDED(dst->QueryInterface(NVRHI_IID_PPV_ARGS(&dstBuffer)))) {
            if (NVRHI_FAILED(src->QueryInterface(NVRHI_IID_PPV_ARGS(&srcBuffer))))
                return nvrhi::FE_INVALID_ARGS;
            size_t numBytes = srcBuffer->getDesc()->byteSize;
            if (numBytes != dstBuffer->getDesc()->byteSize) return nvrhi::FE_INVALID_ARGS;

            Buffer *srcBuffer1 = checked_cast<Buffer>(srcBuffer.Get());
            Buffer *dstBuffer1 = checked_cast<Buffer>(dstBuffer.Get());
            V_CUDA(m_context, cuMemcpyAsync(dstBuffer1->m_cuBuffer, srcBuffer1->m_cuBuffer,
                                            numBytes, m_cuStream));
            return nvrhi::FS_OK;
        }
    }

    {
        nvrhi::AutoPtr<ITexture> srcTexture, dstTexture;
        if (NVRHI_SUCCEEDED(dst->QueryInterface(NVRHI_IID_PPV_ARGS(&dstTexture)))) {
            if (NVRHI_FAILED(src->QueryInterface(NVRHI_IID_PPV_ARGS(&srcTexture))))
                return nvrhi::FE_INVALID_ARGS;

            Texture *dstTexture1 = checked_cast<Texture>(dstTexture.Get());
            Texture *srcTexture1 = checked_cast<Texture>(srcTexture.Get());
            const auto dstDesc = dstTexture1->getDesc();
            const auto srcDesc = srcTexture1->getDesc();
            if (dstDesc->dimension != srcDesc->dimension) return nvrhi::FE_INVALID_ARGS;

            uint32_t dstElemWidth = getFormatInfo(dstDesc->format).bytesPerBlock;
            uint32_t srcElemWidth = getFormatInfo(srcDesc->format).bytesPerBlock;

            if (dstElemWidth != srcElemWidth) return nvrhi::FE_INVALID_ARGS;

            switch (dstDesc->dimension) {
                case TextureDimension::Texture1D:
                    if (dstDesc->width != srcDesc->width ||
                        dstDesc->depthOrArraySize != srcDesc->depthOrArraySize ||
                        dstDesc->mipLevels != srcDesc->mipLevels)
                        return nvrhi::FE_INVALID_ARGS;
                    break;
                case TextureDimension::Texture2D:
                    if (dstDesc->width != srcDesc->width ||
                        dstDesc->height != srcDesc->height ||
                        dstDesc->depthOrArraySize != srcDesc->depthOrArraySize ||
                        dstDesc->mipLevels != srcDesc->mipLevels)
                        return nvrhi::FE_INVALID_ARGS;
                    break;
                case TextureDimension::Texture3D:
                    if (dstDesc->width != srcDesc->width ||
                        dstDesc->height != srcDesc->height ||
                        dstDesc->depthOrArraySize != srcDesc->depthOrArraySize)
                        return nvrhi::FE_INVALID_ARGS;
            }

            CUDA_MEMCPY3D copyInfo = {};
            copyInfo.dstMemoryType = CU_MEMORYTYPE_ARRAY;
            copyInfo.srcMemoryType = CU_MEMORYTYPE_ARRAY;
            for (uint32_t i = 0; i < dstDesc->mipLevels; ++i) {
                V_CUDA(m_context, dstTexture1->levelArray(i, &copyInfo.dstArray));
                V_CUDA(m_context, srcTexture1->levelArray(i, &copyInfo.srcArray));
                CUDA_ARRAY3D_DESCRIPTOR arrayDesc;
                V_CUDA(m_context, cuArray3DGetDescriptor(&arrayDesc, copyInfo.dstArray));
                copyInfo.WidthInBytes = arrayDesc.Width * dstElemWidth;
                copyInfo.Height = arrayDesc.Height;
                copyInfo.Depth = arrayDesc.Depth;
                V_CUDA(m_context, cuMemcpy3DAsync(&copyInfo, m_cuStream));
            }
        }
    }

    return nvrhi::FS_OK;
}

nvrhi::FRESULT DeviceQueue::copyTextureRegion(const TextureCopyLocation &dst, uint32_t dstX,
                                       uint32_t dstY, uint32_t dstZ,
                                       const TextureCopyLocation &src,
                                       const GPBox *srcBox) {

    if (!dst.resource || !src.resource)
        return nvrhi::FE_INVALID_ARGS;

    if (dst.type == TextureCopyType::PlacedFootprint) {
        nvrhi::AutoPtr<IBuffer> dstBuffer;
        if(NVRHI_FAILED(dst.resource->QueryInterface(NVRHI_IID_PPV_ARGS(&dstBuffer))))
            return nvrhi::FE_INVALID_ARGS;
        
        if (src.type == TextureCopyType::PlacedFootprint) {
            // Copy buffer to buffer
            nvrhi::AutoPtr<IBuffer> srcBuffer;
            if (NVRHI_FAILED(src.resource->QueryInterface(NVRHI_IID_PPV_ARGS(&srcBuffer))))
                return nvrhi::FE_INVALID_ARGS;

            return copyBufferRegion(dstBuffer, dst.placeFootprint.offset, srcBuffer,
                             src.placeFootprint.offset, src.placeFootprint.width);
        } else {
            // Copy texture to buffer
            nvrhi::AutoPtr<ITexture> srcTexture;
            if(NVRHI_FAILED(src.resource->QueryInterface(NVRHI_IID_PPV_ARGS(&srcTexture))))
                return nvrhi::FE_INVALID_ARGS;

            auto srcTexture1 = checked_cast<Texture>(srcTexture.Get());
            auto dstBuffer1 = checked_cast<Buffer>(dstBuffer.Get());

            auto srcDesc = srcTexture1->getDesc();
            uint32_t mipIndex, arrayIndex;
            CUarray srcSlice;

            if(srcDesc->dimension == TextureDimension::Texture3D) {
                mipIndex = src.subresourceIndex;
                arrayIndex = 0;
            } else {
                mipIndex = src.subresourceIndex / srcDesc->depthOrArraySize;
                arrayIndex = src.subresourceIndex - srcDesc->depthOrArraySize * mipIndex;
            }
            V_CUDA(m_context, srcTexture1->levelArray(mipIndex, &srcSlice));

            CUDA_ARRAY3D_DESCRIPTOR arrayDesc;
            V_CUDA(m_context, cuArray3DGetDescriptor(&arrayDesc, srcSlice));

            GPBox srcBox1 = {};
            if(srcBox) {
                srcBox1 = *srcBox;
            } else {
                srcBox1.left = srcBox1.top = srcBox1.front = 0;
                srcBox1.right = arrayDesc.Width;
                srcBox1.bottom = arrayDesc.Height;
                if(srcDesc->dimension == TextureDimension::Texture3D)
                    srcBox1.back = arrayDesc.Depth;
            }

            switch (srcDesc->dimension) {
                case TextureDimension::Texture1D:
                    srcBox1.top = srcBox1.bottom = 0;
                    srcBox1.front = arrayIndex;
                    srcBox1.back = arrayIndex + 1;

                    if (srcBox1.left >= srcBox1.right ||
                        srcBox1.right > arrayDesc.Width ||
                        srcBox1.front >= srcBox1.back ||
                        srcBox1.bottom > arrayDesc.Depth)
                        return nvrhi::FE_INVALID_ARGS;
                    break;
                case TextureDimension::Texture2D:
                    srcBox1.front = arrayIndex;
                    srcBox1.back = arrayIndex + 1;

                    if (srcBox1.left >= srcBox1.right ||
                        srcBox1.right > arrayDesc.Width ||
                        srcBox1.top >= srcBox1.bottom ||
                        srcBox1.bottom > arrayDesc.Height ||
                        srcBox1.front >= srcBox1.back ||
                        srcBox1.bottom > arrayDesc.Depth)
                        break;
                default: break;;
            }

            auto elementBytes = getFormatInfo(srcDesc->format).bytesPerBlock;
            uint32_t widthInbytes =
                uint32_t(elementBytes) * (srcBox1.right - srcBox1.left);
            uint32_t height = (srcBox1.bottom - srcBox1.top);
            uint32_t depth = (srcBox1.back - srcBox1.front);

            CUDA_MEMCPY3D copyInfo = {};
            copyInfo.srcMemoryType = CU_MEMORYTYPE_ARRAY;
            copyInfo.srcArray = srcSlice;
            copyInfo.srcXInBytes = srcBox1.left * elementBytes;
            copyInfo.srcY = srcBox1.top;
            copyInfo.srcZ = srcBox1.front;
            copyInfo.WidthInBytes = widthInbytes;
            copyInfo.Height = height;
            copyInfo.Depth = depth;

            auto dstElementBytes = getFormatInfo(dst.placeFootprint.format).bytesPerBlock;

            copyInfo.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            copyInfo.dstDevice = dstBuffer1->m_cuBuffer;
            copyInfo.dstPitch = dst.placeFootprint.rowPitch;
            if(srcDesc->dimension == TextureDimension::Texture3D)
                copyInfo.dstHeight = dst.placeFootprint.height;

            copyInfo.dstXInBytes = dst.placeFootprint.offset + dstX * dstElementBytes;
            copyInfo.dstY = dstY;
            copyInfo.dstZ = dstZ;

            V_CUDA(m_context, cuMemcpy3DAsync(&copyInfo, m_cuStream));
        }
    } else {
        nvrhi::AutoPtr<ITexture> dstTexture;
        if(NVRHI_FAILED(dst.resource->QueryInterface(NVRHI_IID_PPV_ARGS(&dstTexture))))
            return nvrhi::FE_INVALID_ARGS;

        Texture *dstTexture1 = checked_cast<Texture>(dstTexture.Get());

        auto dstDesc = dstTexture1->getDesc();
        uint32_t dstMipIndex, dstArrayIndex = 0;
        CUarray dstArray;

        if (dstDesc->dimension == TextureDimension::Texture3D) {
            dstMipIndex = dst.subresourceIndex;
        } else {
            dstMipIndex = dst.subresourceIndex / dstDesc->depthOrArraySize;
            dstArrayIndex = dst.subresourceIndex - dstDesc->depthOrArraySize * dstMipIndex;
        }
        V_CUDA(m_context, dstTexture1->levelArray(dstMipIndex, &dstArray));

        CUDA_ARRAY3D_DESCRIPTOR dstArrayDesc;
        V_CUDA(m_context, cuArray3DGetDescriptor(&dstArrayDesc, dstArray));

        uint32_t dstElementBytes = getFormatInfo(dstDesc->format).bytesPerBlock;

        if(src.type == TextureCopyType::PlacedFootprint) {
            // Copy buffer to texture
            nvrhi::AutoPtr<IBuffer> srcBuffer;
            if(NVRHI_FAILED(src.resource->QueryInterface(NVRHI_IID_PPV_ARGS(&srcBuffer))))
                return nvrhi::FE_INVALID_ARGS;

            Buffer *srcBuffer1 = checked_cast<Buffer>(srcBuffer.Get());

            GPBox srcBox1;
            if(srcBox) {
                srcBox1 = *srcBox;
            } else {
                srcBox1.left = srcBox1.top = srcBox1.front = 0;
                srcBox1.right = src.placeFootprint.width;
                srcBox1.bottom = src.placeFootprint.height;
                srcBox1.back = src.placeFootprint.depth;
            }

            switch (dstDesc->dimension) {
                case TextureDimension::Texture1D:
                    srcBox1.top = srcBox1.bottom = 0;
                    srcBox1.front = 0;
                    srcBox1.back = 1;
                    break;
                case TextureDimension::Texture2D:
                    srcBox1.front = 0;
                    srcBox1.back = 1;
                    break;
                default:
                    break;
            }

            CUDA_MEMCPY3D copyInfo = {};
            copyInfo.dstMemoryType = CU_MEMORYTYPE_ARRAY;
            copyInfo.dstArray = dstArray;
            copyInfo.dstXInBytes = dstX * dstElementBytes;
            copyInfo.dstY = dstY;
            if (dstDesc->dimension == TextureDimension::Texture3D) copyInfo.dstZ = dstZ;
            else
                copyInfo.dstZ = dstArrayIndex;

            uint32_t srcElementBytes =
                getFormatInfo(src.placeFootprint.format).bytesPerBlock;

            copyInfo.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            copyInfo.srcDevice = srcBuffer1->m_cuBuffer;
            copyInfo.srcXInBytes =
                src.placeFootprint.offset + srcBox1.left * srcElementBytes;
            copyInfo.srcY = srcBox1.top;
            copyInfo.srcZ = srcBox1.front;
            copyInfo.srcPitch = src.placeFootprint.rowPitch;
            copyInfo.srcHeight = src.placeFootprint.height;

            copyInfo.WidthInBytes = (srcBox1.right - srcBox1.left) * srcElementBytes;
            copyInfo.Height = (srcBox1.bottom - srcBox1.top);
            copyInfo.Depth = (srcBox1.back - srcBox1.front);

            V_CUDA(m_context, cuMemcpy3DAsync(&copyInfo, m_cuStream));
        } else {
            // Copy texture to texture
            nvrhi::AutoPtr<ITexture> srcTexture;
            if (NVRHI_FAILED(src.resource->QueryInterface(NVRHI_IID_PPV_ARGS(&srcTexture))))
                return nvrhi::FE_INVALID_ARGS;

            Texture *srcTexture1 = checked_cast<Texture>(srcTexture.Get());
            auto srcDesc = srcTexture1->getDesc();
            uint32_t srcElementBytes = getFormatInfo(dstDesc->format).bytesPerBlock;

            if(srcDesc->dimension != dstDesc->dimension || srcElementBytes != dstElementBytes)
                return nvrhi::FE_INVALID_ARGS;

            uint32_t srcMipIndex, srcArrayIndex = 0;
            CUarray srcArray;

            if (srcDesc->dimension == TextureDimension::Texture3D) {
                srcMipIndex = src.subresourceIndex;
            } else {
                srcMipIndex = src.subresourceIndex / srcDesc->depthOrArraySize;
                srcArrayIndex =
                    src.subresourceIndex - srcDesc->depthOrArraySize * srcMipIndex;
            }
            V_CUDA(m_context, srcTexture1->levelArray(srcMipIndex, &srcArray));

            CUDA_ARRAY3D_DESCRIPTOR srcArrayDesc;
            V_CUDA(m_context, cuArray3DGetDescriptor(&srcArrayDesc, srcArray));

            // The source is a subresource location: the default box is the
            // whole array level, not a footprint.
            GPBox srcBox1;
            if (srcBox) {
                srcBox1 = *srcBox;
            } else {
                srcBox1.left = srcBox1.top = srcBox1.front = 0;
                srcBox1.right = uint32_t(srcArrayDesc.Width);
                srcBox1.bottom = uint32_t(std::max<size_t>(srcArrayDesc.Height, 1));
                srcBox1.back = uint32_t(std::max<size_t>(srcArrayDesc.Depth, 1));
            }

            switch (srcDesc->dimension) {
                case TextureDimension::Texture1D:
                    srcBox1.top = 0;
                    srcBox1.bottom = 1;
                    srcBox1.front = srcArrayIndex;
                    srcBox1.back = srcArrayIndex + 1;
                    break;
                case TextureDimension::Texture2D:
                    srcBox1.front = srcArrayIndex;
                    srcBox1.back = srcArrayIndex + 1;
                    break;
                default:
                    break;
            }

            CUDA_MEMCPY3D copyInfo = {};
            copyInfo.dstMemoryType = CU_MEMORYTYPE_ARRAY;
            copyInfo.dstArray = dstArray;
            copyInfo.dstXInBytes = dstX * dstElementBytes;
            copyInfo.dstY = dstY;
            copyInfo.dstZ = (dstDesc->dimension == TextureDimension::Texture3D) ? dstZ : dstArrayIndex;

            copyInfo.srcMemoryType = CU_MEMORYTYPE_ARRAY;
            copyInfo.srcArray = srcArray;
            copyInfo.srcXInBytes = srcBox1.left * srcElementBytes;
            copyInfo.srcY = srcBox1.top;
            copyInfo.srcZ = srcBox1.front;
            copyInfo.WidthInBytes = (srcBox1.right - srcBox1.left) * srcElementBytes;
            copyInfo.Height = (srcBox1.bottom - srcBox1.top);
            copyInfo.Depth = (srcBox1.back - srcBox1.front);

            V_CUDA(m_context, cuMemcpy3DAsync(&copyInfo, m_cuStream));
        }
    }

    return nvrhi::FS_OK;
}

nvrhi::FRESULT DeviceQueue::writeTextureRegion(IResource *dstResource, uint32_t dstSubresource,
                                        const void *srcData,
                                        const SubresourceFootprint &footprint,
                                        uint32_t dstX, uint32_t dstY, uint32_t dstZ,
                                        const GPBox *srcBox) {
    if(dstResource == nullptr || srcData == nullptr ||
        footprint.format == Format::UNKNOWN || footprint.width == 0 ||
        footprint.height == 0 || footprint.depth == 0)
        return nvrhi::FE_INVALID_ARGS;

    nvrhi::AutoPtr<IBuffer> stagingBuffer;
    nvrhi::FRESULT fr;

    BufferDesc bufDesc = {};
    bufDesc.byteSize = uint64_t(footprint.rowPitch) * footprint.height * footprint.depth;
    bufDesc.isStaging = true;
    if (NVRHI_FAILED(fr = m_context.device->createBuffer(bufDesc, &stagingBuffer))) return fr;

    auto buffer = checked_cast<Buffer>(stagingBuffer.Get());
    m_stagingBuffers.push_back({m_lastSyncPoint, buffer});
    memcpy((void *)buffer->m_cuBuffer, srcData, bufDesc.byteSize);

    TextureCopyLocation dstLocation;
    dstLocation.resource = dstResource;
    dstLocation.type = TextureCopyType::SubresourceIndex;
    dstLocation.subresourceIndex = dstSubresource;
    TextureCopyLocation srcLocation;
    srcLocation.resource = buffer;
    srcLocation.type = TextureCopyType::PlacedFootprint;
    srcLocation.placeFootprint.offset = 0;
    srcLocation.placeFootprint.format = footprint.format;
    srcLocation.placeFootprint.width = footprint.width;
    srcLocation.placeFootprint.height = footprint.height;
    srcLocation.placeFootprint.depth = footprint.depth;
    srcLocation.placeFootprint.rowPitch = footprint.rowPitch;
    return copyTextureRegion(dstLocation, dstX, dstY, dstZ, srcLocation, srcBox);
}

nvrhi::FRESULT DeviceQueue::acquireInteropKeyedMutexes(
    const GraphicsInteropKeyedMutexWaitParams *waitParamsArray, uint32_t numParamsArray) {
#ifdef _WIN32
    if (numParamsArray == 0) return nvrhi::FS_OK;

    std::vector<CUexternalSemaphore> mutexes;
    std::vector<CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS> paramsSet;
    CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS params = {};

    for (uint32_t i = 0; i < numParamsArray; ++i) {
        auto _resource = waitParamsArray[i].resource;
        nvrhi::AutoPtr<IBuffer> buffer;
        nvrhi::AutoPtr<ITexture> texture;
        if (NVRHI_FAILED(_resource->QueryInterface(NVRHI_IID_PPV_ARGS(&buffer))) &&
            NVRHI_FAILED(_resource->QueryInterface(NVRHI_IID_PPV_ARGS(&texture)))) {
            VERROR(m_context, "IResource is not any kind of IBuffer or ITexture");
            return nvrhi::FE_INVALID_ARGS;
        }
        if(buffer) {
            auto buffer2 = checked_cast<Buffer>(buffer.Get());

            if (buffer2->m_cuD3D11KeyedMutex == 0)
                continue;

            if(mutexes.empty()) {
                mutexes.reserve(numParamsArray);
                paramsSet.reserve(numParamsArray);
            }

            mutexes.push_back(buffer2->m_cuD3D11KeyedMutex);
            params.params.keyedMutex.key = waitParamsArray[i].key;
            params.params.keyedMutex.timeoutMs = waitParamsArray[i].timeoutMS;
            paramsSet.push_back(params);
        } else {
            auto texture2 = checked_cast<Texture>(texture.Get());
            if (texture2->m_cuD3D11KeyedMutex == 0) continue;

            if(mutexes.empty()) {
                mutexes.reserve(numParamsArray);
                paramsSet.reserve(numParamsArray);
            }

            mutexes.push_back(texture2->m_cuD3D11KeyedMutex);
            params.params.keyedMutex.key = waitParamsArray[i].key;
            params.params.keyedMutex.timeoutMs = waitParamsArray[i].timeoutMS;
            paramsSet.push_back(params);
        }
    }

    if (!mutexes.empty()) {
        CUresult cr = cuWaitExternalSemaphoresAsync(mutexes.data(), paramsSet.data(),
                                                    mutexes.size(), m_cuStream);
        if (cr == CUDA_ERROR_TIMEOUT)
            return nvrhi::FE_WAIT_TIMEOUT;
        else if (cr != CUDA_SUCCESS) {
            V_CUDA(m_context, cr);
            return nvrhi::FE_INVALID_ARGS;
        }
    }
#endif

    return nvrhi::FS_OK;
}

nvrhi::FRESULT DeviceQueue::releaseInteropKeyedMutexes(const GraphicsInteropKeyedMutexSignalParams *signalParamsArray, uint32_t numParamsArray) {
#ifdef _WIN32
    if (numParamsArray == 0) return nvrhi::FS_OK;

    std::vector<CUexternalSemaphore> mutexes;
    std::vector<CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS> paramsSet;
    CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS params = {};

    for (uint32_t i = 0; i < numParamsArray; ++i) {
        auto _resource = signalParamsArray[i].resource;
        nvrhi::AutoPtr<IBuffer> buffer;
        nvrhi::AutoPtr<ITexture> texture;
        if (NVRHI_FAILED(_resource->QueryInterface(NVRHI_IID_PPV_ARGS(&buffer))) &&
            NVRHI_FAILED(_resource->QueryInterface(NVRHI_IID_PPV_ARGS(&texture)))) {
            VERROR(m_context, "IResource is not any kind of IBuffer or ITexture");
            return nvrhi::FE_INVALID_ARGS;
        }
        if (buffer) {
            auto buffer2 = checked_cast<Buffer>(buffer.Get());

            if (buffer2->m_cuD3D11KeyedMutex == 0) continue;

            if (mutexes.empty()) {
                mutexes.reserve(numParamsArray);
                paramsSet.reserve(numParamsArray);
            }

            mutexes.push_back(buffer2->m_cuD3D11KeyedMutex);
            params.params.keyedMutex.key = signalParamsArray[i].key;
            paramsSet.push_back(params);
        } else {
            auto texture2 = checked_cast<Texture>(texture.Get());
            if (texture2->m_cuD3D11KeyedMutex == 0) continue;

            if (mutexes.empty()) {
                mutexes.reserve(numParamsArray);
                paramsSet.reserve(numParamsArray);
            }

            mutexes.push_back(texture2->m_cuD3D11KeyedMutex);
            params.params.keyedMutex.key = signalParamsArray[i].key;
            paramsSet.push_back(params);
        }
    }

    if (!mutexes.empty()) {
        CUresult cr = cuSignalExternalSemaphoresAsync(mutexes.data(), paramsSet.data(),
                                                    mutexes.size(), m_cuStream);
        if (cr != CUDA_SUCCESS) {
            V_CUDA(m_context, cr);
            return nvrhi::FE_INVALID_ARGS;
        }
    }
#endif

    return nvrhi::FS_OK;
}

nvrhi::FRESULT DeviceQueue::signalInteropSemaphore(IGraphicsInteropSemaphore *_semaphore, uint64_t value) {
    if (!_semaphore) return nvrhi::FE_INVALID_ARGS;

    auto semaphore = checked_cast<GraphicsInteropSemaphore>(_semaphore);

    CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS params = {};
    params.params.fence.value = value;

    CUresult cr =
        cuSignalExternalSemaphoresAsync(&semaphore->m_cuSemaphore, &params, 1, m_cuStream);
    if(cr != CUDA_SUCCESS) {
        V_CUDA(m_context, cr);
        return nvrhi::FE_GENERIC_ERROR;
    }

    return nvrhi::FS_OK;
}

nvrhi::FRESULT DeviceQueue::waitInteropSemaphoreAsync(IGraphicsInteropSemaphore *_semaphore, uint64_t value) {
    if (!_semaphore) return nvrhi::FE_INVALID_ARGS;

    auto semaphore = checked_cast<GraphicsInteropSemaphore>(_semaphore);

    CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS params = {};
    params.params.fence.value = value;

    CUresult cr =
        cuWaitExternalSemaphoresAsync(&semaphore->m_cuSemaphore, &params, 1, m_cuStream);
    if(cr != CUDA_SUCCESS) {
        V_CUDA(m_context, cr);
        return nvrhi::FE_GENERIC_ERROR;
    }

    return nvrhi::FS_OK;
}

DeviceQueue::DeviceQueue(Context &context, const DeviceQueueDesc &desc)
    : DeviceChild<IDeviceQueue>(context), m_desc(desc), m_cuStream(0), m_lastSyncPoint{0} {
    int priority;
    switch (m_desc.priority) {
        case DeviceQueuePriority::Highest:
            priority = 0;
            break;
        case DeviceQueuePriority::AboveNormal:
            priority = 1;
            break;
        default:
        case DeviceQueuePriority::Normal:
            priority = 2;
            break;
        case DeviceQueuePriority::BelowNormal:
            priority = 3;
            break;
        case DeviceQueuePriority::Lowest:
            priority = 4;
            break;
    }

    V_CUDA(m_context,
           cuStreamCreateWithPriority(&m_cuStream, CU_STREAM_NON_BLOCKING, priority));
}

DeviceQueue::~DeviceQueue() {
    m_context.device->waitForQueue(this);
    V_CUDA(m_context, cuStreamDestroy(m_cuStream));
    m_cuStream = 0;
}

void DeviceQueue::clearStagingResources(uint64_t syncPoint) {
    for (auto it = m_stagingBuffers.begin(); it != m_stagingBuffers.end();) {
        if (it->syncPoint <= syncPoint) {
            it = m_stagingBuffers.erase(it);
        } else
            ++it;
    }
}

Device::Device(const Context &context) : m_context(context), m_lastSyncPoint{} {
    m_context.msgCallback->AddRef();
    m_context.device = this;
}

Device::~Device() {
    for (auto &syncPoint : m_syncEvents)
        V_CUDA(m_context, cuEventDestroy(syncPoint.event));
    m_syncEvents.clear();

    V_CUDA(m_context, cuCtxDestroy(m_context.cuContext));
    m_context.msgCallback->Release();
}

nvrhi::FRESULT Device::createBuffer(const BufferDesc &desc, IBuffer **_buffer) {
    auto buffer = MAKE_RC_OBJ(Buffer, m_context, desc);
    if (_buffer) {
        *_buffer = buffer;
        buffer->AddRef();
    }
    buffer->Release();
    return nvrhi::FS_OK;
}

nvrhi::FRESULT Device::createTexture(const TextureDesc &desc, ITexture **_texture) {
    auto texture = MAKE_RC_OBJ(Texture, m_context, desc);
    if (_texture) {
        *_texture = texture;
        texture->AddRef();
    }
    texture->Release();
    return nvrhi::FS_OK;
}

nvrhi::FRESULT Device::createModule(const ModuleDesc &desc, const void *data, size_t dataSize,
                             IModule **_lib) {
    auto pModule = MAKE_RC_OBJ(Module, m_context, desc, data, dataSize);
    if (_lib) {
        *_lib = pModule;
        pModule->AddRef();
    }
    pModule->Release();
    return nvrhi::FS_OK;
}

nvrhi::FRESULT Device::createDeviceQueue(const DeviceQueueDesc &desc, IDeviceQueue **_queue) {
    auto pQueue = MAKE_RC_OBJ(DeviceQueue, m_context, desc);
    if (_queue) {
        *_queue = pQueue;
        pQueue->AddRef();
    }
    pQueue->Release();
    return nvrhi::FS_OK;
}

void Device::commitQueue(IDeviceQueue *_queue) {
    auto queue = checked_cast<DeviceQueue>(_queue);
    if (!queue) return;

    auto syncPoint = allocateSyncPoint(queue, false);
    queue->m_lastSyncPoint = syncPoint.point;
    V_CUDA(m_context, cuEventRecord(syncPoint.event, queue->m_cuStream));
}

nvrhi::FRESULT Device::waitForQueue(IDeviceQueue *_queue) {
    auto queue = checked_cast<DeviceQueue>(_queue);
    if (!queue) return nvrhi::FE_INVALID_ARGS;

    if (queue->m_lastSyncPoint == 0) {
        // No commitQueue called
        V_CUDA(m_context, cuStreamSynchronize(queue->m_cuStream));
        queue->clearStagingResources(m_lastSyncPoint);
        return nvrhi::FS_OK;
    } else {
        std::lock_guard<std::mutex> guard{m_syncEventAllocMtx};
        for (auto it = m_syncEvents.begin(); it != m_syncEvents.end();) {
            if (it->queue == queue && it->point <= queue->m_lastSyncPoint) {
                V_CUDA(m_context, cuEventSynchronize(it->event));
                V_CUDA(m_context, cuEventDestroy(it->event));
                it = m_syncEvents.erase(it);
            } else
                ++it;
        }
        queue->clearStagingResources(queue->m_lastSyncPoint);
    }
    return nvrhi::FS_OK;
}

void Device::waitForIdle() { V_CUDA(m_context, cuCtxSynchronize()); }

Module::Module(Context &context, const ModuleDesc &desc, const void *data, size_t dataSize)
    : DeviceChild<IModule>(context), m_desc(desc), m_cuModule{} {
    if (data && dataSize) V_CUDA(m_context, cuModuleLoadData(&m_cuModule, data));
}

Module::~Module() {
    if (m_cuModule) {
        V_CUDA(m_context, cuModuleUnload(m_cuModule));
        m_cuModule = 0;
    }
}

nvrhi::FRESULT Module::getKernel(const char *sysName, IKernel **ppKernel) {
    if (sysName == nullptr) return nvrhi::FE_INVALID_ARGS;

    auto it = m_kernelLibs.find(sysName);
    if (it != m_kernelLibs.end()) {
        if (ppKernel) {
            *ppKernel = &it->second;
            it->second.AddRef();
        }
        return nvrhi::FS_OK;
    }

    CUfunction func;
    CUresult rc = cuModuleGetFunction(&func, m_cuModule, sysName);
    if (rc != CUDA_SUCCESS) {
        V_CUDA(m_context, rc);
        return nvrhi::FE_NOT_FOUND;
    }
    // NOTE(migration): donut's nvrhi::DelegatingObjectImpl deletes its copy/move constructors
    // (the old ethereal fork did not), so Kernel can no longer be moved into the map.
    // Construct it in place instead - semantics are unchanged.
    auto itres = m_kernelLibs.try_emplace(sysName, this, m_context, sysName, func);
    itres.first->second.m_funcName = itres.first->first.c_str(); // fixup string reference.
    if (ppKernel) {
        *ppKernel = &itres.first->second;
        (*ppKernel)->AddRef();
    }
    return nvrhi::FS_OK;
}

bool Module::getConstant(const char *name, CUdeviceptr *dptr, size_t *bytes) {
    auto it = std::find_if(m_constants.begin(), m_constants.end(),
                           [name](const ConstantInfo &info) { return info.name == name; });
    if (it != m_constants.end()) {
        *dptr = it->dptr;
        *bytes = it->bytes;
        return true;
    }
    CUresult rc = cuModuleGetGlobal(dptr, bytes, m_cuModule, name);
    if (rc != CUDA_SUCCESS) {
        V_CUDA(m_context, rc);
        return false;
    }
    ConstantInfo info = {name, *dptr, *bytes};
    m_constants.push_back(std::move(info));
    return true;
}

Kernel::Kernel(nvrhi::IObject *pOwner, Context &context, const char *funcName, CUfunction func)
    : nvrhi::DelegatingObjectImpl<IKernel>(pOwner),
      m_context(context),
      m_funcName(funcName),
      m_cuFunc(func) {}

Kernel::~Kernel() {}

IDevice *Kernel::getDevice() { return static_cast<Module *>(m_pOwner)->getDevice(); }

IModule *Kernel::getModule() {
    auto pOwner = static_cast<Module *>(m_pOwner);
    return pOwner;
}

const char *Kernel::getName() { return m_funcName; }

nvrhi::FRESULT DeviceQueue::setConstantBuffer(IKernel *pKernel, const char *symName, void *data,
                                 size_t dataSize) {
    if (pKernel == nullptr || data == nullptr || dataSize == 0) return nvrhi::FE_INVALID_ARGS;

    auto pKernelCUDA = checked_cast<Kernel>(pKernel);

    nvrhi::FRESULT res;
    auto pModule = static_cast<Module *>(pKernelCUDA->getModule());
    CUdeviceptr dptr;
    size_t dsize;
    if (!pModule->getConstant(symName, &dptr, &dsize)) return nvrhi::FE_NOT_FOUND;

    if (dataSize < dsize) return nvrhi::FE_INVALID_ARGS;

    BufferDesc stagingBufferDesc;
    stagingBufferDesc.isStaging = true;
    stagingBufferDesc.byteSize = dsize;
    Buffer *stagingBuffer;
    m_context.device->createBuffer(stagingBufferDesc, (IBuffer **)&stagingBuffer);
    m_stagingBuffers.push_back({m_lastSyncPoint, stagingBuffer});
    stagingBuffer->Release();

    memcpy((void *)stagingBuffer->m_cuBuffer, data, dataSize);

    CUresult cures =
        cuMemcpyHtoDAsync(dptr, (void *)stagingBuffer->m_cuBuffer, dsize, m_cuStream);
    if (cures != CUDA_SUCCESS) {
        V_CUDA(m_context, cures);
        return nvrhi::FE_GENERIC_ERROR;
    }

    return nvrhi::FS_OK;
}

nvrhi::FRESULT DeviceQueue::setConstantBuffer2(IKernel *pKernel, const char *symName,
                                        IBuffer *_buffer, uint64_t offset) {
    if (pKernel == nullptr || _buffer == nullptr
        || _buffer->getDesc()->byteSize <= offset) return nvrhi::FE_INVALID_ARGS;

    auto buffer = checked_cast<Buffer>(_buffer);
    auto pKernelCUDA = checked_cast<Kernel>(pKernel);

    nvrhi::FRESULT res;
    auto pModule = static_cast<Module *>(pKernelCUDA->getModule());
    CUdeviceptr dptr;
    size_t dsize;
    if (!pModule->getConstant(symName, &dptr, &dsize)) return nvrhi::FE_NOT_FOUND;

    if (offset + dsize > buffer->getDesc()->byteSize) return nvrhi::FE_INVALID_ARGS;

    CUresult cures =
        cuMemcpyAsync(dptr, (CUdeviceptr)((uint8_t *)buffer->m_cuBuffer + offset), dsize, m_cuStream);
    if (cures != CUDA_SUCCESS) {
        V_CUDA(m_context, cures);
        return nvrhi::FE_GENERIC_ERROR;
    }
    return nvrhi::FS_OK;
}

SyncPointInfo Device::allocateSyncPoint(DeviceQueue *queue, bool syncWithQueue) {
    uint64_t nextSyncPoint =
        m_lastSyncPoint.fetch_add(1, std::memory_order::memory_order_relaxed) + 1;
    {
        std::lock_guard<std::mutex> allocGuard{m_syncEventAllocMtx};
        CUevent event;
        V_CUDA(m_context, cuEventCreate(&event, CU_EVENT_BLOCKING_SYNC));
        SyncPointInfo info{nextSyncPoint, event, queue, syncWithQueue};
        m_syncEvents.push_back(info);
        return info;
    }
}

nvrhi::FRESULT Device::mapBuffer(IBuffer *_buffer, void **data) {
    auto buffer = checked_cast<Buffer>(_buffer);
    if (buffer == nullptr || !buffer->m_desc.isStaging) return nvrhi::FE_INVALID_ARGS;

    if (data) *data = (void *)buffer->m_cuBuffer;
    return nvrhi::FS_OK;
}

void Device::unmapBuffer(IBuffer *_buffer) {}

nvrhi::FRESULT Device::createInteropD3D11Buffer(ID3D11Resource *_buffer, uint64_t mappedOffset,
                                         uint64_t mappedSize, IBuffer **ppBuffer) {
    nvrhi::AutoPtr<ID3D11Buffer> d3d11Buffer;
    if (!_buffer || FAILED(_buffer->QueryInterface(IID_PPV_ARGS(&d3d11Buffer))))
        return nvrhi::FE_INVALID_ARGS;

    D3D11_BUFFER_DESC d3d11BufDesc;
    d3d11Buffer->GetDesc(&d3d11BufDesc);
    if (!(d3d11BufDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) ||
        mappedOffset >= d3d11BufDesc.ByteWidth ||
        (mappedOffset + mappedSize) > d3d11BufDesc.ByteWidth)
        return nvrhi::FE_INVALID_ARGS;

    HRESULT hr;
    nvrhi::AutoPtr<IDXGIResource1> dxgiResource;
    if (FAILED(hr = d3d11Buffer->QueryInterface(IID_PPV_ARGS(&dxgiResource)))) {
        VERROR(m_context, "[GPDevice] ID3D11Buffer query IDXGIResource1 error: %#08X", hr);
        return nvrhi::FE_GENERIC_ERROR;
    }

    HANDLE sharedHandle;
    if (FAILED(hr = dxgiResource->CreateSharedHandle(
                   NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL,
                   &sharedHandle))) {
        VERROR(m_context, "[GPDevice] ID3D11Buffer CreateSharedHandle error: %#08X", hr);
        return nvrhi::FE_GENERIC_ERROR;
    }

    if (mappedSize == 0) mappedSize = d3d11BufDesc.ByteWidth - mappedOffset;

    GraphicsInteropBufferDesc interopDesc = {};
    interopDesc.graphicsAPI = GraphicsInteropAPI::D3D11;
    interopDesc.memorySize = d3d11BufDesc.ByteWidth;
    interopDesc.bufferSize = mappedSize;
    interopDesc.bufferOffset = mappedOffset;
    interopDesc.handle = (void *)sharedHandle;
    interopDesc.d3d11KeyedMutexHandle = (void *)sharedHandle;

    auto buffer = MAKE_RC_OBJ(Buffer, m_context, interopDesc);
    if (ppBuffer) {
        *ppBuffer = buffer;
        buffer->AddRef();
    }
    buffer->Release();

    return nvrhi::FS_OK;
}

nvrhi::FRESULT Device::createInteropD3D12Buffer(ID3D12Resource *d3d12Buffer, uint64_t mappedOffset,
                                         uint64_t mappedSize, IBuffer **ppBuffer) {
    if (!d3d12Buffer) return nvrhi::FE_INVALID_ARGS;

    D3D12_RESOURCE_DESC d3d12BufDesc = d3d12Buffer->GetDesc();
    nvrhi::AutoPtr<ID3D12Device> d3d12Device;
    d3d12Buffer->GetDevice(IID_PPV_ARGS(&d3d12Device));
    auto allocInfo = d3d12Device->GetResourceAllocationInfo(0, 1, &d3d12BufDesc);

    // The mapped range lies inside the buffer (Width); the imported memory is
    // the whole allocation, which is rounded up to the heap alignment.
    if (mappedSize == 0 && mappedOffset < d3d12BufDesc.Width) mappedSize = d3d12BufDesc.Width - mappedOffset;
    if (mappedSize == 0 || mappedOffset >= d3d12BufDesc.Width || (mappedOffset + mappedSize) > d3d12BufDesc.Width)
        return nvrhi::FE_INVALID_ARGS;

    // A D3D12 resource is shared through ID3D12Device::CreateSharedHandle (it
    // has no IDXGIResource1; the resource must live on a shared heap).
    HRESULT hr;
    HANDLE sharedHandle;
    if (FAILED(hr = d3d12Device->CreateSharedHandle(d3d12Buffer, NULL, GENERIC_ALL, 0,
                                                    &sharedHandle))) {
        VERROR(m_context, "[GPDevice] ID3D12Device::CreateSharedHandle (buffer) error: %#08X", hr);
        return nvrhi::FE_GENERIC_ERROR;
    }

    GraphicsInteropBufferDesc interopDesc = {};
    interopDesc.graphicsAPI = GraphicsInteropAPI::D3D12;
    interopDesc.memorySize = allocInfo.SizeInBytes;
    interopDesc.bufferSize = mappedSize;
    interopDesc.bufferOffset = mappedOffset;
    interopDesc.handle = (void *)sharedHandle;

    auto buffer = MAKE_RC_OBJ(Buffer, m_context, interopDesc);
    if (ppBuffer) {
        *ppBuffer = buffer;
        buffer->AddRef();
    }
    buffer->Release();

    return nvrhi::FS_OK;
}

nvrhi::FRESULT Device::createInteropVulkanBuffer(void *_vkBuffer, void *_vkMemory, void *_vkDevice,
                                          uint64_t mappedOffset, uint64_t mappedSize,
                                          IBuffer **ppBuffer) {
    if (!_vkBuffer || !_vkMemory || !_vkDevice) return nvrhi::FE_INVALID_ARGS;

    vk::Buffer vkBuffer{(VkBuffer)_vkBuffer};
    vk::DeviceMemory vkMemory{(VkDeviceMemory)_vkMemory};
    vk::Device vkDevice{(VkDevice)_vkDevice};

    vk::MemoryRequirements memReq;
    vkDevice.getBufferMemoryRequirements(vkBuffer, &memReq);
    if (mappedOffset >= memReq.size || (mappedOffset % memReq.alignment) != 0 ||
        mappedOffset + mappedSize > memReq.size)
        return nvrhi::FE_INVALID_ARGS;

    void *opaqueHandle;

#ifdef _WIN32
    vk::MemoryGetWin32HandleInfoKHR memHandleInfo = {};
    memHandleInfo.setHandleType(vk::ExternalMemoryHandleTypeFlagBits::eOpaqueWin32)
        .setMemory(vkMemory);
    HANDLE win32Handle;
    vk::Result vkRet;

    if ((vkRet = vkDevice.getMemoryWin32HandleKHR(&memHandleInfo, &win32Handle)) !=
        vk::Result::eSuccess) {
        VERROR(m_context, "[GPDevice] vkGetMemoryWin32HandleKHR failed: %u",
               (uint32_t)vkRet);
        return nvrhi::FE_GENERIC_ERROR;
    }
    opaqueHandle = (void *)win32Handle;
#elif defined(__unix__) || defined(__linux__)
    VkMemoryGetFdInfoKHR memHandleInfo = {};
    memHandleInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    memHandleInfo.pNext = NULL;
    memHandleInfo.memory = vkMemory;
    memHandleInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkResult vkRet;
    int fd = -1;

    if ((vkRet = vkGetMemoryFdKHR(vkDevice, &memHandleInfo, &fd)) != VK_SUCCESS) {
        VERROR(m_context, "[GPDevice] vkGetMemoryFdKHR failed: %u", (uint32_t)vkRet);
        return nvrhi::FE_GENERIC_ERROR;
    }
    opaqueHandle = (void *)fd;
#else
#error "Unsupported platform!"
#endif

    if (mappedSize == 0) mappedSize = memReq.size - mappedOffset;

    GraphicsInteropBufferDesc interopDesc = {};
    interopDesc.graphicsAPI = GraphicsInteropAPI::Vulkan;
    interopDesc.handle = opaqueHandle;
    interopDesc.memorySize = memReq.size;
    interopDesc.bufferOffset = mappedOffset;
    interopDesc.bufferSize = mappedSize;

    Buffer *buffer = MAKE_RC_OBJ(Buffer, m_context, interopDesc);
    if (ppBuffer) {
        *ppBuffer = buffer;
        buffer->AddRef();
    }
    buffer->Release();
    return nvrhi::FS_OK;
}

nvrhi::FRESULT Device::createInteropD3D11Texture(ID3D11Resource *_d3d11Resource,
                                          ITexture **ppTexture) {
    if (!_d3d11Resource) return nvrhi::FE_INVALID_ARGS;

    nvrhi::AutoPtr<ID3D11Texture1D> d3d11Tex1D;
    nvrhi::AutoPtr<ID3D11Texture2D> d3d11Tex2D;
    nvrhi::AutoPtr<ID3D11Texture3D> d3d11Tex3D;
    HRESULT hr;

    if (FAILED(hr = _d3d11Resource->QueryInterface(IID_PPV_ARGS(&d3d11Tex1D))) &&
        FAILED(hr = _d3d11Resource->QueryInterface(IID_PPV_ARGS(&d3d11Tex2D))) &&
        FAILED(hr = _d3d11Resource->QueryInterface(IID_PPV_ARGS(&d3d11Tex3D)))) {
        VERROR(m_context, "Failed to query interface of ID3D11Texture[123]D, hr = %#08X",
               hr);
        return nvrhi::FE_GENERIC_ERROR;
    }

    nvrhi::AutoPtr<IDXGIResource1> dxgiResource;
    TextureDesc texDesc;
    DXGI_FORMAT dxgiFormat;
    uint64_t memSize;

    if (d3d11Tex1D) {
        D3D11_TEXTURE1D_DESC tex1DDesc;
        d3d11Tex1D->GetDesc(&tex1DDesc);
        texDesc.dimension = TextureDimension::Texture1D;
        texDesc.width = tex1DDesc.Width;
        texDesc.depthOrArraySize = tex1DDesc.ArraySize;
        // texDesc.mipLevels = tex1DDesc.MipLevels;
        dxgiFormat = tex1DDesc.Format;

        memSize = tex1DDesc.Width * tex1DDesc.ArraySize;
    } else if (d3d11Tex2D) {
        D3D11_TEXTURE2D_DESC tex2DDesc;
        d3d11Tex2D->GetDesc(&tex2DDesc);
        texDesc.dimension = TextureDimension::Texture2D;
        texDesc.width = tex2DDesc.Width;
        texDesc.height = tex2DDesc.Height;
        texDesc.depthOrArraySize = tex2DDesc.ArraySize;
        // texDesc.mipLevels = tex2DDesc.MipLevels;
        dxgiFormat = tex2DDesc.Format;

        memSize = tex2DDesc.Width * tex2DDesc.Height * tex2DDesc.ArraySize;
    } else {
        D3D11_TEXTURE3D_DESC tex3DDesc;
        d3d11Tex3D->GetDesc(&tex3DDesc);
        texDesc.dimension = TextureDimension::Texture3D;
        texDesc.width = tex3DDesc.Width;
        texDesc.height = tex3DDesc.Height;
        texDesc.depthOrArraySize = tex3DDesc.Depth;
        texDesc.mipLevels = 1;
        dxgiFormat = tex3DDesc.Format;

        memSize = tex3DDesc.Width * tex3DDesc.Height * tex3DDesc.Depth;
    }

    if (!getFormatFromDXGIFormat(dxgiFormat, &texDesc.format)) {
        VERROR(m_context, "DXGI format can not map to GPDevice format");
        return nvrhi::FE_GENERIC_ERROR;
    }

    auto &formatInfo = getFormatInfo(texDesc.format);
    memSize = memSize * formatInfo.bytesPerBlock;

    if (FAILED(hr = _d3d11Resource->QueryInterface(IID_PPV_ARGS(&dxgiResource)))) {
        VERROR(m_context, "Failed to query interface of IDXGIResource1, hr = %#08X", hr);
        return nvrhi::FE_GENERIC_ERROR;
    }

    HANDLE win32Handle;
    if (FAILED(
            hr = dxgiResource->CreateSharedHandle(NULL, GENERIC_ALL, NULL, &win32Handle))) {
        VERROR(m_context, "IDXGIResource1::CreateSharedResource failed, hr=%#08X", hr);
        return nvrhi::FE_GENERIC_ERROR;
    }

    GraphicsInteropTextureDesc interopDesc = {};
    interopDesc.graphicsAPI = GraphicsInteropAPI::D3D11;
    interopDesc.handle = (void *)win32Handle;
    interopDesc.memorySize = memSize;
    interopDesc.baseMipLevelMemoryOffset = 0;
    interopDesc.numMipLevels = 1; // TODO: accept D3D11 texture with all mipmaps
    interopDesc.d3d11KeyedMutexHandle = (void *)win32Handle;

    auto texture = MAKE_RC_OBJ(Texture, m_context, interopDesc, texDesc);
    if (ppTexture) {
        *ppTexture = texture;
        texture->AddRef();
    }
    texture->Release();

    return nvrhi::FS_OK;
}

nvrhi::FRESULT Device::createInteropD3D12Texture(ID3D12Resource *d3d12Texture,
                                          ITexture **ppTexture) {
    if (!d3d12Texture) return nvrhi::FE_INVALID_ARGS;

    D3D12_RESOURCE_DESC d3d12ResDesc = d3d12Texture->GetDesc();
    ID3D12Device *d3d12Device;
    HRESULT hr;

    d3d12Texture->GetDevice(IID_PPV_ARGS(&d3d12Device));
    d3d12Device->Release();

    HANDLE win32Handle;
    if (FAILED(hr = d3d12Device->CreateSharedHandle(d3d12Texture, NULL, GENERIC_ALL, NULL,
                                                    &win32Handle))) {
        VERROR(m_context, "ID3D12Device::CreateSharedHandle failed, hr=%#08X", hr);
        return nvrhi::FE_GENERIC_ERROR;
    }

    auto allocationInfo = d3d12Device->GetResourceAllocationInfo(0, 1, &d3d12ResDesc);

    TextureDesc texDesc = {};
    GraphicsInteropTextureDesc interopDesc;
    interopDesc.graphicsAPI = GraphicsInteropAPI::D3D12;
    interopDesc.handle = (void *)win32Handle;
    interopDesc.memorySize = allocationInfo.SizeInBytes;
    interopDesc.baseMipLevelMemoryOffset = 0;
    interopDesc.numMipLevels = 1;

    switch (d3d12ResDesc.Dimension) {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            texDesc.width = d3d12ResDesc.Width;
            texDesc.depthOrArraySize = d3d12ResDesc.DepthOrArraySize;
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
            texDesc.width = d3d12ResDesc.Width;
            texDesc.height = d3d12ResDesc.Height;
            texDesc.depthOrArraySize = d3d12ResDesc.DepthOrArraySize;
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            texDesc.width = d3d12ResDesc.Width;
            texDesc.height = d3d12ResDesc.Height;
            texDesc.depthOrArraySize = d3d12ResDesc.DepthOrArraySize;
            break;
        default:
            VERROR(m_context, "ID3D12Resource is not kind of Texture[123]D");
            return nvrhi::FE_GENERIC_ERROR;
    }

    if (!getFormatFromDXGIFormat(d3d12ResDesc.Format, &texDesc.format)) {
        VERROR(m_context, "DXGI format cannot map to a GPDevice format");
        return nvrhi::FE_GENERIC_ERROR;
    }

    auto texture = MAKE_RC_OBJ(Texture, m_context, interopDesc, texDesc);
    if (ppTexture) {
        *ppTexture = texture;
        texture->AddRef();
    }
    texture->Release();
    return nvrhi::FS_OK;
}

nvrhi::FRESULT Device::createInteropVulkanTexture(void *_vkImage, void *_vkMemory, void *_vkDevice,
                                           const TextureDesc &textureDesc,
                                           ITexture **ppTexture) {
    if (!_vkImage || !_vkMemory || !_vkDevice) return nvrhi::FE_INVALID_ARGS;

    vk::Image vkImage{(VkImage)_vkImage};
    vk::DeviceMemory vkMemory{(VkDeviceMemory)_vkMemory};
    vk::Device vkDevice{(VkDevice)_vkDevice};

    vk::MemoryRequirements memReq;
    vkDevice.getImageMemoryRequirements(vkImage, &memReq);

    void *opaqueHandle;

#ifdef _WIN32
    vk::MemoryGetWin32HandleInfoKHR memHandleInfo = {};
    memHandleInfo.setHandleType(vk::ExternalMemoryHandleTypeFlagBits::eOpaqueWin32)
        .setMemory(vkMemory);
    HANDLE win32Handle;
    vk::Result vkRet;

    if ((vkRet = vkDevice.getMemoryWin32HandleKHR(&memHandleInfo, &win32Handle)) !=
        vk::Result::eSuccess) {
        VERROR(m_context, "[GPDevice] vkGetMemoryWin32HandleKHR failed: %u",
               (uint32_t)vkRet);
        return nvrhi::FE_GENERIC_ERROR;
    }
    opaqueHandle = (void *)win32Handle;
#elif defined(__unix__) || defined(__linux__)
    VkMemoryGetFdInfoKHR memHandleInfo = {};
    memHandleInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    memHandleInfo.pNext = NULL;
    memHandleInfo.memory = vkMemory;
    memHandleInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkResult vkRet;
    int fd = -1;

    if ((vkRet = vkGetMemoryFdKHR(vkDevice, &memHandleInfo, &fd)) != VK_SUCCESS) {
        VERROR(m_context, "[GPDevice] vkGetMemoryFdKHR failed: %u", (uint32_t)vkRet);
        return nvrhi::FE_GENERIC_ERROR;
    }
    opaqueHandle = (void *)fd;
#else
#error "Unsupported platform!"
#endif

    GraphicsInteropTextureDesc interopDesc = {};
    interopDesc.graphicsAPI = GraphicsInteropAPI::Vulkan;
    interopDesc.handle = opaqueHandle;
    interopDesc.memorySize = memReq.size;
    interopDesc.baseMipLevelMemoryOffset = 0;
    interopDesc.numMipLevels = 1;

    auto texture = MAKE_RC_OBJ(Texture, m_context, interopDesc, textureDesc);
    if (ppTexture) {
        *ppTexture = texture;
        texture->AddRef();
    }
    texture->Release();

    return nvrhi::FS_OK;
}

nvrhi::FRESULT Device::createInteropD3D11Fence(ID3D11Fence *d3d11Fence,
                                        IGraphicsInteropSemaphore **_semaphore) {
    if (!d3d11Fence) return nvrhi::FE_INVALID_ARGS;

    HANDLE sharedHandle;
    HRESULT hr;

    if(FAILED(hr = d3d11Fence->CreateSharedHandle(NULL, GENERIC_ALL, NULL, &sharedHandle))) {
        VERROR(m_context, "ID3D11Fence::CreateSharedHandle failed, hr=%#08X", hr);
        return nvrhi::FE_GENERIC_ERROR;
    }

    GraphicsInteropSemaphoreDesc desc = {};
    desc.graphicsAPI = GraphicsInteropAPI::D3D11;
    desc.handle = (void *)sharedHandle;
    desc.initValue = d3d11Fence->GetCompletedValue();

    auto semaphore = MAKE_RC_OBJ(GraphicsInteropSemaphore, m_context, desc);
    if(_semaphore) {
        *_semaphore = semaphore;
        semaphore->AddRef();
    }
    semaphore->Release();
    return nvrhi::FS_OK;
}

nvrhi::FRESULT Device::createInteropD3D12Fence(ID3D12Fence *d3d12Fence,
                                        IGraphicsInteropSemaphore **_semaphore) {
    if (!d3d12Fence) return nvrhi::FE_INVALID_ARGS;

    HANDLE sharedHandle;
    HRESULT hr;
    ID3D12Device *d3d12Device;
    d3d12Fence->GetDevice(IID_PPV_ARGS(&d3d12Device));
    d3d12Device->Release();

    if(FAILED(hr = d3d12Device->CreateSharedHandle(d3d12Fence, NULL, GENERIC_ALL, NULL, &sharedHandle))) {
        VERROR(m_context, "ID3D12Device::CreateSharedHandle failed, hr=%#08X", hr);
        return nvrhi::FE_GENERIC_ERROR;
    }

    GraphicsInteropSemaphoreDesc desc = {};
    desc.graphicsAPI = GraphicsInteropAPI::D3D12;
    desc.handle = (void *)sharedHandle;
    desc.initValue = d3d12Fence->GetCompletedValue();

    auto semaphore = MAKE_RC_OBJ(GraphicsInteropSemaphore, m_context, desc);
    if(_semaphore) {
        *_semaphore = semaphore;
        semaphore->AddRef();
    }
    semaphore->Release();
    return nvrhi::FS_OK;
}

nvrhi::FRESULT Device::createInteropVulkanSemaphore(void *_vkSemaphore, void *_vkDevice,
                                             IGraphicsInteropSemaphore **_semaphore) {
    if (!_vkSemaphore || !_vkDevice) return nvrhi::FE_INVALID_ARGS;

    vk::Device vkDevice((VkDevice)_vkDevice);
    vk::Semaphore vkSemaphore((VkSemaphore)_vkSemaphore);
    vk::Result vkRet;
    GraphicsInteropSemaphoreDesc desc = {};
    desc.graphicsAPI = GraphicsInteropAPI::Vulkan;

#ifdef _WIN32
    vk::SemaphoreGetWin32HandleInfoKHR win32HandleInfo;
    HANDLE win32Handle;
    win32HandleInfo.setHandleType(vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueWin32)
        .setSemaphore(vkSemaphore);

    vkRet = vkDevice.getSemaphoreWin32HandleKHR(&win32HandleInfo, &win32Handle);
    if (vkRet != vk::Result::eSuccess) {
        VERROR(m_context, "vkGetSemaphoreWin32HandleKHR failed, error code=%u\n", vkRet);
        return nvrhi::FE_GENERIC_ERROR;
    }

    desc.handle = (void *)win32Handle;
#elif defined(__unix__) || defined(__linux__)

    vk::SemaphoreGetFdInfoKHR fdInfo;
    int fd;
    fdInfo.setHandleType(vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd)
        .setSemaphore(vkSemaphore);

    vkRet = vkDevice.getSemaphoreFdKHR(&fdInfo, &fd);
    if (vkRet != vk::Result::eSuccess) {
        VERROR(m_context, "vkGetSemaphoreFdKHR failed, error code=%u\n", vkRet);
        return nvrhi::FE_GENERIC_ERROR;
    }

    desc.handle = (void *)fd;
#endif

    desc.initValue = vkDevice.getSemaphoreCounterValue(vkSemaphore);

    auto semaphore = MAKE_RC_OBJ(GraphicsInteropSemaphore, m_context, desc);
    if(_semaphore) {
        *_semaphore = semaphore;
        semaphore->AddRef();
    }
    semaphore->Release();
    return nvrhi::FS_OK;
}

GraphicsInteropSemaphore::GraphicsInteropSemaphore(
    Context &context, const GraphicsInteropSemaphoreDesc &desc): DeviceChild<IGraphicsInteropSemaphore>(context), m_desc{desc}, m_cuSemaphore{} {
    CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC semDesc = {};

    switch (m_desc.graphicsAPI) {
        case GraphicsInteropAPI::D3D11:
            semDesc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE;
            semDesc.handle.win32.handle = m_desc.handle;
            break;
        case GraphicsInteropAPI::D3D12:
            semDesc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE;
            semDesc.handle.win32.handle = m_desc.handle;
            break;
        case GraphicsInteropAPI::Vulkan:
#ifdef _WIN32
            semDesc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TIMELINE_SEMAPHORE_WIN32;
            semDesc.handle.win32.handle = m_desc.handle;
#elif defined(__unix__) || defined(__linux__)
            semDesc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TIMELINE_SEMAPHORE_FD;
            semDesc.handle.fd = (int)m_desc.handle;
            break;
#else
#error "Unsupported platform!"
#endif
            break;
    }

    V_CUDA(m_context, cuImportExternalSemaphore(&m_cuSemaphore, &semDesc));
}

GraphicsInteropSemaphore::~GraphicsInteropSemaphore() {
    if (m_cuSemaphore) {
        V_CUDA(m_context, cuDestroyExternalSemaphore(m_cuSemaphore));
        m_cuSemaphore = 0;
    }
}

}  // namespace donut::gp::cuda
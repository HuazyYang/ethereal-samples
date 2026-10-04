// NvVolumetricLighting.cpp
//
// Exported entry points, global library state, default allocator / assert handler and the global
// allocation operators.

#include "Common.h"
#include "ContextImp.h"
#include "d3d11/ContextImp_D3D11.h"

#include <stdlib.h>

namespace Nv
{
namespace VolumetricLighting
{

namespace
{

// Default allocator used when OpenLibrary receives a null allocator.
// NvVolumetricLighting.d3d11.dll: vtable 0x1801FB828 (allocate 0x1800052D0, deallocate 0x180005300),
// static instance 0x1801FFAE0 constructed by 0x180001000.
class DefaultAllocator : public nvidia::NvAllocatorCallback
{
public:
    void* allocate(size_t size, const char* /*typeName*/, const char* /*filename*/, int /*line*/) override
    {
        return malloc(size);
    }

    void deallocate(void* ptr) override
    {
        free(ptr);
    }
};

// Default assert handler: ignores every assertion.
// NvVolumetricLighting.d3d11.dll: vtable 0x1801FB840 (operator() 0x180005530 is empty),
// static instance 0x1801FFAD8 constructed by 0x180001030.
class DefaultAssertHandler : public nvidia::NvAssertHandler
{
public:
    void operator()(const char* /*exp*/, const char* /*file*/, int /*line*/, bool& /*ignore*/) override
    {
    }
};

DefaultAllocator s_defaultAllocator;
DefaultAssertHandler s_defaultAssertHandler;

} // namespace

// NvVolumetricLighting.d3d11.dll: 0x1801FFAC0, 0x1801FFAC8, 0x1801FFAD0
nvidia::NvAllocatorCallback* g_allocator = nullptr;
nvidia::NvAssertHandler* g_assertHandler = nullptr;
bool g_isLibraryOpen = false;

// NvVolumetricLighting.d3d11.dll: 0x180005610
Status OpenLibrary(nvidia::NvAllocatorCallback* allocator, nvidia::NvAssertHandler* assertion_handler,
                   const VersionDesc& version)
{
    if (version.Major != 1 || version.Minor != 0)
    {
        return Status::INVALID_VERSION;
    }
    g_allocator = (allocator != nullptr) ? allocator : &s_defaultAllocator;
    g_assertHandler = (assertion_handler != nullptr) ? assertion_handler : &s_defaultAssertHandler;
    g_isLibraryOpen = true;
    return Status::OK;
}

// NvVolumetricLighting.d3d11.dll: 0x1800056B0
Status CloseLibrary()
{
    if (!g_isLibraryOpen)
    {
        return Status::UNINITIALIZED;
    }
    g_isLibraryOpen = false;
    return Status::OK;
}

// NvVolumetricLighting.d3d11.dll: 0x1800056D0
Status CreateContext(Context& out_ctx, const PlatformDesc* pPlatformDesc, const ContextDesc* pContextDesc)
{
    if (!g_isLibraryOpen)
    {
        return Status::UNINITIALIZED;
    }
    if (pPlatformDesc == nullptr || pContextDesc == nullptr)
    {
        return Status::INVALID_PARAMETER;
    }
    out_ctx = nullptr;
    if (pPlatformDesc->platform != PlatformName::D3D11)
    {
        return Status::INVALID_PARAMETER;
    }

    ContextImp_D3D11* ctx = nullptr;
    Status status = ContextImp_D3D11::Create(&ctx, pPlatformDesc, pContextDesc);
    if (status != Status::OK)
    {
        return status;
    }
    out_ctx = static_cast<ContextImp*>(ctx);
    return Status::OK;
}

// NvVolumetricLighting.d3d11.dll: 0x180005780
Status ReleaseContext(Context& ctx)
{
    if (ctx == nullptr)
    {
        return Status::INVALID_PARAMETER;
    }
    delete static_cast<ContextImp*>(ctx);
    ctx = nullptr;
    return Status::OK;
}

// NvVolumetricLighting.d3d11.dll: 0x180005800
Status BeginAccumulation(Context ctx, BeginAccumulationArgs* pArgs)
{
    return static_cast<ContextImp*>(ctx)->BeginAccumulation(pArgs);
}

// NvVolumetricLighting.d3d11.dll: 0x180005830
Status RenderVolume(Context ctx, RenderVolumeArgs* pArgs)
{
    return static_cast<ContextImp*>(ctx)->RenderVolume(pArgs);
}

// NvVolumetricLighting.d3d11.dll: 0x180005860
Status EndAccumulation(Context ctx, EndAccumulationArgs* pArgs)
{
    return static_cast<ContextImp*>(ctx)->EndAccumulation(pArgs);
}

// NvVolumetricLighting.d3d11.dll: 0x180005890
Status ApplyLighting(Context ctx, ApplyLightingArgs* pArgs)
{
    return static_cast<ContextImp*>(ctx)->ApplyLighting(pArgs);
}

} // namespace VolumetricLighting
} // namespace Nv

namespace nvidia
{

// NvVolumetricLighting.d3d11.dll: 0x180005600
NvAssertHandler& NvGetAssertHandler()
{
    return *Nv::VolumetricLighting::g_assertHandler;
}

} // namespace nvidia

////////////////////////////////////////////////////////////////////////////////
// Global allocation operators (see Common.h)

// NvVolumetricLighting.d3d11.dll: 0x1800053D0
void* operator new(size_t size, const char* filename, int line)
{
    return Nv::VolumetricLighting::g_allocator->allocate(size, "Gameworks Volumetric Lighting", filename, line);
}

// NvVolumetricLighting.d3d11.dll: 0x180005420 (only reached when a constructor throws)
void operator delete(void* ptr, const char* /*filename*/, int /*line*/)
{
    Nv::VolumetricLighting::g_allocator->deallocate(ptr);
}

// NvVolumetricLighting.d3d11.dll: 0x180005460 (the sized delete 0x18000F180 forwards here)
void operator delete(void* ptr) noexcept
{
    Nv::VolumetricLighting::g_allocator->deallocate(ptr);
}

// NvVolumetricLighting.d3d11.dll: 0x1800054A0
void* operator new[](size_t size, const char* filename, int line)
{
    return Nv::VolumetricLighting::g_allocator->allocate(size, "Gameworks Volumetric Lighting", filename, line);
}

void operator delete[](void* ptr, const char* /*filename*/, int /*line*/)
{
    Nv::VolumetricLighting::g_allocator->deallocate(ptr);
}

// NvVolumetricLighting.d3d11.dll: 0x1800054F0 (the sized delete[] 0x18000F084 forwards here)
void operator delete[](void* ptr) noexcept
{
    Nv::VolumetricLighting::g_allocator->deallocate(ptr);
}

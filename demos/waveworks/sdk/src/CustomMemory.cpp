#include "CustomMemory.h"

#include <cstdlib>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace
{
void* defaultMalloc(size_t size)
{
    return std::malloc(size);
}

void defaultFree(void* ptr)
{
    std::free(ptr);
}

void* defaultAlignedMalloc(size_t size, size_t alignment)
{
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    if (alignment < sizeof(void*))
        alignment = sizeof(void*);

    void* ptr = nullptr;
    return posix_memalign(&ptr, alignment, size) == 0 ? ptr : nullptr;
#endif
}

void defaultAlignedFree(void* ptr)
{
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}
}

void (*NVSDK_aligned_free)(void*) = defaultAlignedFree;
void* (*NVSDK_aligned_malloc)(size_t, size_t) = defaultAlignedMalloc;
void (*NVSDK_free)(void*) = defaultFree;
void* (*NVSDK_malloc)(size_t) = defaultMalloc;

void internalFree(void *p)
{
    NVSDK_free(p);
}

void *internalMalloc(size_t sz)
{
    void *result = NVSDK_malloc(sz);
    if (!result)
        diagnostic_message(L"WaveWorks: MEMORY ALLOCATION ERROR. Check memory allocation callback pointer\n");
    return result;
}

void resetMemoryManagementCallbacksToDefaults()
{
    NVSDK_malloc = defaultMalloc;
    NVSDK_free = defaultFree;
    NVSDK_aligned_malloc = defaultAlignedMalloc;
    NVSDK_aligned_free = defaultAlignedFree;
}

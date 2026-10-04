// Common.h
//
// Library-wide helpers: allocator routing (global operator new/delete replacements), release
// macros and the global library state set by OpenLibrary.

#pragma once

#include <Nv/VolumetricLighting/NvVolumetricLighting.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace Nv
{
namespace VolumetricLighting
{

// Global library state (NvVolumetricLighting.d3d11.dll: 0x1801FFAC0 / 0x1801FFAC8 / 0x1801FFAD0).
extern nvidia::NvAllocatorCallback* g_allocator;
extern nvidia::NvAssertHandler* g_assertHandler;
extern bool g_isLibraryOpen;

} // namespace VolumetricLighting
} // namespace Nv

// Allocation routing.
//
// The DLL replaces the global array/scalar delete operators and provides placement operator new
// overloads taking (file, line); every object of the library is created with NV_NEW and destroyed
// with a plain delete, so all memory goes through the NvAllocatorCallback given to OpenLibrary.
// The "file" argument is always the module name and the line is 0.
//   operator new(size_t, const char*, int)      NvVolumetricLighting.d3d11.dll: 0x1800053D0
//   operator delete(void*, const char*, int)    NvVolumetricLighting.d3d11.dll: 0x180005420
//   operator delete(void*)                      NvVolumetricLighting.d3d11.dll: 0x180005460
//   operator new[](size_t, const char*, int)    NvVolumetricLighting.d3d11.dll: 0x1800054A0
//   operator delete[](void*)                    NvVolumetricLighting.d3d11.dll: 0x1800054F0
void* operator new(size_t size, const char* filename, int line);
void operator delete(void* ptr, const char* filename, int line);
void* operator new[](size_t size, const char* filename, int line);
void operator delete[](void* ptr, const char* filename, int line);

#define NV_NEW new ("NvVolumetricLighting.dll", 0)

#define SAFE_RELEASE(x)          \
    if ((x) != nullptr)          \
    {                            \
        (x)->Release();          \
        (x) = nullptr;           \
    }

// Releases every element of a fixed-size array. NOTE: the original applies this macro to the
// heap-allocated shader tables (T** pointers), where sizeof(x)/sizeof(x[0]) == 1, so only the first
// permutation of each table is released when a context is destroyed. Reproduced as-is.
#define SAFE_RELEASE_ARRAY(x)                                          \
    for (unsigned int i__ = 0; i__ < sizeof(x) / sizeof((x)[0]); ++i__) \
    {                                                                  \
        SAFE_RELEASE((x)[i__]);                                        \
    }

#define SAFE_DELETE(x)    \
    if ((x) != nullptr)   \
    {                     \
        delete (x);       \
        (x) = nullptr;    \
    }

#define V_RETURN(expr)                      \
    {                                       \
        Status status__ = (expr);           \
        if (status__ != Status::OK)         \
        {                                   \
            return status__;                \
        }                                   \
    }

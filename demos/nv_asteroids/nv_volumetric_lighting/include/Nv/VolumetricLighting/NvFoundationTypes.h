// NvFoundationTypes.h
//
// Minimal foundation types used by the NvVolumetricLighting public API:
//   - nvidia::NvAllocatorCallback / nvidia::NvAssertHandler (NvFoundation style interfaces)
//   - NvcVec2/3/4 and NvcMat44 (NvCTypes style plain C vector types)
//
// The original library ships these in separate NvFoundation / NvCTypes headers which could not be
// retrieved for this reconstruction; the declarations below are the minimal versions required for
// binary compatibility with NvVolumetricLighting.d3d11.dll:
//   - virtual method order is fixed by the calls made by the DLL
//     (allocator vtable: [0] dtor, [1] allocate(size, typeName, filename, line), [2] deallocate(ptr);
//      assert handler vtable: [0] dtor, [1] operator()(exp, file, line, ignore)),
//   - class/namespace names are fixed by the exported mangled names
//     (?OpenLibrary@...@@YA?AW4Status@12@PEAVNvAllocatorCallback@nvidia@@PEAVNvAssertHandler@5@AEBUVersionDesc@12@@Z,
//      ?NvGetAssertHandler@nvidia@@YAAEAVNvAssertHandler@1@XZ).

#ifndef NV_FOUNDATION_TYPES_H
#define NV_FOUNDATION_TYPES_H

#include <stddef.h>
#include <stdint.h>

namespace nvidia
{

// Abstract memory allocation interface. The library routes every allocation through the callback
// passed to OpenLibrary (or a malloc/free based default).
class NvAllocatorCallback
{
public:
    virtual ~NvAllocatorCallback() {}
    virtual void* allocate(size_t size, const char* typeName, const char* filename, int line) = 0;
    virtual void deallocate(void* ptr) = 0;
};

// Assertion handler interface (the library's default handler ignores assertions).
class NvAssertHandler
{
public:
    virtual ~NvAssertHandler() {}
    virtual void operator()(const char* exp, const char* file, int line, bool& ignore) = 0;
};

} // namespace nvidia

// Plain C vector/matrix types.
// NvcMat44 stores four columns (column-vector convention, v' = M * v). The memory layout is
// identical to a row-major matrix used with row vectors (D3DX / DirectXMath style), which is how the
// DLL consumes the matrices (they are copied unchanged into the shader constant buffers).
struct NvcVec2
{
    float x, y;
};

struct NvcVec3
{
    float x, y, z;
};

struct NvcVec4
{
    float x, y, z, w;
};

struct NvcMat44
{
    NvcVec4 column0;
    NvcVec4 column1;
    NvcVec4 column2;
    NvcVec4 column3;
};

#endif // NV_FOUNDATION_TYPES_H

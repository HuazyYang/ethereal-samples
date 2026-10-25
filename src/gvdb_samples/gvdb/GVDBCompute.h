#ifndef GVDB_GVDBCOMPUTE_H
#define GVDB_GVDBCOMPUTE_H
// The compute layer GVDB runs on: the nvrhi CUDA device and Slang kernels.
//
//   IComputeContext  the CUDA device, the command list the work is recorded
//                    into (submitted by flush(), waited for by finish()) and
//                    the kernels of the shader containers of a file system
//   IComputeKernel   one CUDA kernel of a Slang module and its pipeline; the
//                    kernel arguments are the entry point's uniform parameters
//
// A volume, its operation objects and the renderers that draw it share one
// context, so their work is ordered on one CUDA stream. Kernels take their
// data through pointers (deviceAddress()) and descriptor handles
// (nvrhi::cuda::IDevice::getTextureHandle / getSurfaceHandle), never through
// binding sets; launchKernel() places the arguments where the reflection of
// the shader container says.
#include <nvrhi/nvrhi.h>
#include <nvrhi/cuda.h>
#include <nvrhi/shader-reflection.h>
#include <donut/core/math/math.h>
#include <cstring>
#include <type_traits>

namespace donut::vfs {
class IFileSystem;
}

namespace gvdb {

NVRHI_IID(IComputeKernel, "27be66fd-4d66-4d85-985a-8ccfb5ea19ea")
struct IComputeKernel : public nvrhi::IObject {
    NVRHI_DECLARE_UUID_TRAITS(IComputeKernel)

    virtual const char *getName() const = 0;
    virtual dm::uint3 getThreadGroupSize() const = 0;
    // The kernel arguments: the uniform parameters of the entry point, in declaration order.
    virtual uint32_t getArgumentCount() const = 0;
    virtual const nvrhi::ShaderParameterReflection *getArgument(uint32_t index) const = 0;
    virtual uint32_t getArgumentsSize() const = 0;
    // The reflection of the shader container the kernel comes from (types of the arguments, ...).
    virtual const nvrhi::ShaderLibraryReflection *getLibraryReflection() const = 0;
    // Records a launch of `groups` thread groups; `arguments` holds getArgumentsSize() bytes.
    virtual void launch(nvrhi::ICommandList *commandList, dm::uint3 groups, const void *arguments) = 0;
};

NVRHI_IID(IComputeContext, "631490d5-3a07-482f-b34e-81ca5546ba6d")
struct IComputeContext : public nvrhi::IObject {
    NVRHI_DECLARE_UUID_TRAITS(IComputeContext)

    virtual nvrhi::cuda::IDevice *getDevice() const = 0;
    virtual donut::vfs::IFileSystem *getFileSystem() const = 0;
    // The open command list GVDB work is recorded into.
    virtual nvrhi::ICommandList *getCommandList() = 0;
    // Submits the recorded work (nothing when nothing was recorded).
    virtual void flush() = 0;
    // Submits the recorded work and waits until the GPU has finished it.
    virtual void finish() = 0;
    // Keeps an object alive until the GPU has finished the work recorded so far: kernels reach buffers
    // and textures through device addresses and descriptor handles, which command lists do not track.
    // Call it before dropping a resource that recorded or submitted work may still use.
    virtual void releaseAfterUse(nvrhi::IObject *object) = 0;
    // Copies `size` bytes of a buffer to host memory; finishes the context first.
    virtual void readBuffer(nvrhi::IBuffer *buffer, uint64_t offset, void *data, uint64_t size) = 0;
    // A kernel of a shader container of the file system (loaded once, cached by path and name).
    virtual IComputeKernel *getKernel(const char *containerPath, const char *entryName) = 0;
    // The kernel of the first container that defines it (an error message when none does).
    virtual IComputeKernel *findKernel(const char *const *containerPaths, uint32_t count, const char *entryName) = 0;
};

nvrhi::FRESULT createComputeContext(nvrhi::cuda::IDevice *device, donut::vfs::IFileSystem *vfs,
                                    IComputeContext **context);

// The device address of a buffer for a pointer argument (0 without a buffer).
inline uint64_t deviceAddress(nvrhi::IBuffer *buffer, uint64_t offset = 0) {
    return buffer ? buffer->getGpuVirtualAddress() + offset : 0;
}

// Thread groups covering `threads` threads with the kernel's thread group size.
dm::uint3 groupsFor(IComputeKernel *kernel, dm::uint3 threads);
inline dm::uint3 groupsFor(IComputeKernel *kernel, uint64_t threads) {
    return groupsFor(kernel, dm::uint3(uint32_t(threads), 1u, 1u));
}

namespace detail {
// Copies argument `index` into the argument block; false (and an error message) when the
// kernel has no such argument or it has another size.
bool writeKernelArgument(IComputeKernel *kernel, uint32_t index, uint8_t *block, const void *value, size_t size);
bool checkKernelArguments(IComputeKernel *kernel, uint32_t count);
constexpr size_t c_MaxKernelArgumentsSize = 4096;   // CUDA's limit of kernel parameter space
}  // namespace detail

// Records a launch of `kernel` with its arguments in parameter order: plain values as
// they are (bool, uint8_t, int3 ...), pointers as deviceAddress() values, textures and
// surfaces as descriptor handles. False (and nothing recorded) when the arguments do not
// match the kernel's parameters.
template <typename... Args>
bool launchKernel(nvrhi::ICommandList *commandList, IComputeKernel *kernel, dm::uint3 groups, const Args &...args) {
    static_assert((std::is_trivially_copyable_v<Args> && ...), "kernel arguments are plain data");
    if (!detail::checkKernelArguments(kernel, uint32_t(sizeof...(Args)))) return false;
    alignas(16) uint8_t block[detail::c_MaxKernelArgumentsSize];
    std::memset(block, 0, kernel->getArgumentsSize());
    uint32_t index = 0;
    bool ok = true;
    ((ok = ok && detail::writeKernelArgument(kernel, index++, block, &args, sizeof(Args))), ...);
    if (!ok) return false;
    kernel->launch(commandList, groups, block);
    return true;
}

}  // namespace gvdb

#endif /* GVDB_GVDBCOMPUTE_H */

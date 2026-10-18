// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <nvrhi/shader-program.h>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <tuple>

namespace ethereal::slang {
class ComputeRuntime {
    struct Kernel {
        nvrhi::ShaderProgramHandle program;
        nvrhi::ShaderEntryPointDesc entryDesc;
        bool raycastLayoutValidated = false;
        nvrhi::BindingLayoutDesc layoutDesc;
        nvrhi::BindingLayoutHandle layout;
        nvrhi::ComputePipelineHandle pipeline;
        std::vector<nvrhi::ShaderParameterDesc> parameters;
    };
    nvrhi::DeviceHandle m_device;
    nvrhi::CommandListHandle m_commands;
    bool m_open = false;
    void ensureOpen();
    std::filesystem::path m_shaderPath;
    std::map<std::string, Kernel> m_kernels;
    std::map<std::tuple<nvrhi::ResourceType,uint32_t,uint32_t>, nvrhi::BufferHandle> m_defaults;
    Kernel& kernel(const char* source, const char* entry);
public:
    ComputeRuntime(nvrhi::IDevice* device, const std::filesystem::path& shaderPath);
    ~ComputeRuntime();
    nvrhi::IDevice* device() const { return m_device; }
    nvrhi::BufferHandle buffer(size_t bytes, uint32_t stride, const void* data = nullptr, bool raw = false);
    void upload(nvrhi::IBuffer* buffer, const void* data, size_t bytes);
    // App raycast ABI guard, before transient output allocation. Rejects typed
    // atlas shaders, extra globals, entry arguments and incompatible constants.
    void requireRaycastKernel(const char* source, const char* entry);
    void dispatch(const char* source, const char* entry, const void* parameters, size_t parameterBytes,
        const std::vector<nvrhi::BindingSetItem>& bindings, uint32_t x, uint32_t y = 1, uint32_t z = 1);
    void flush();
    void read(nvrhi::IBuffer* buffer, void* data, size_t bytes);
    nvrhi::BufferHandle exclusiveScan(nvrhi::IBuffer* input, uint32_t count);
    nvrhi::BufferHandle radixSort(nvrhi::IBuffer* input, uint32_t count);
    nvrhi::BufferHandle reduce(nvrhi::IBuffer* input, uint32_t count);
};
} // namespace ethereal::slang

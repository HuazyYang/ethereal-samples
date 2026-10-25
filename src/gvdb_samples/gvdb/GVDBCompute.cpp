// The compute layer of GVDB (see GVDBCompute.h): one command list on the
// nvrhi CUDA device, and kernels loaded from NVRHI shader containers.
#include <gvdb/GVDBCompute.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <nvrhi/core/datablob.h>
#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace gvdb {

using namespace donut;

namespace {

class ComputeKernel final : public nvrhi::ObjectImpl<IComputeKernel> {
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ComputeKernel)
    NVRHI_IMPLEMENTS_INTERFACE(IComputeKernel)
    NVRHI_END_INTERFACE_TABLE()

    // False (and an error message) when the shader has no reflection or the pipeline fails.
    bool initialize(nvrhi::IDevice *device, nvrhi::IShader *shader, const char *name) {
        m_name = name;
        m_shader = shader;
        m_entryPoint = shader->getReflection();
        m_library = shader->getLibraryReflection();
        if (!m_entryPoint || !m_library) {
            log::error("GVDB: kernel %s has no reflection (compile it into an NVRHI shader container)", name);
            return false;
        }
        m_argumentsSize = m_entryPoint->paramsSize;
        if (m_argumentsSize > detail::c_MaxKernelArgumentsSize) {
            log::error("GVDB: kernel %s takes %u bytes of arguments (more than %zu)", name, m_argumentsSize,
                       detail::c_MaxKernelArgumentsSize);
            return false;
        }

        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.setComputeShader(shader);
        if (m_argumentsSize) {
            // The kernel arguments are the push constants of the CUDA backend.
            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.setVisibility(nvrhi::ShaderType::Compute)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0, m_argumentsSize));
            if (NVRHI_FAILED(device->createBindingLayout(layoutDesc, &m_layout)) ||
                NVRHI_FAILED(device->createBindingSet(
                    nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0, m_argumentsSize)),
                    m_layout, &m_bindingSet))) {
                log::error("GVDB: cannot create the bindings of kernel %s", name);
                return false;
            }
            pipelineDesc.addBindingLayout(m_layout);
        }
        if (NVRHI_FAILED(device->createComputePipeline(pipelineDesc, &m_pipeline))) {
            log::error("GVDB: cannot create the pipeline of kernel %s", name);
            return false;
        }
        return true;
    }

    const char *getName() const override { return m_name.c_str(); }

    dm::uint3 getThreadGroupSize() const override {
        return dm::max(dm::uint3(m_entryPoint->threadGroupSize[0], m_entryPoint->threadGroupSize[1],
                                 m_entryPoint->threadGroupSize[2]),
                       dm::uint3(1u));
    }

    uint32_t getArgumentCount() const override { return m_entryPoint->parameterCount; }

    const nvrhi::ShaderParameterReflection *getArgument(uint32_t index) const override {
        if (index >= m_entryPoint->parameterCount) return nullptr;
        return &m_library->parameters[m_entryPoint->firstParameter + index];
    }

    uint32_t getArgumentsSize() const override { return m_argumentsSize; }
    const nvrhi::ShaderLibraryReflection *getLibraryReflection() const override { return m_library; }

    void launch(nvrhi::ICommandList *commandList, dm::uint3 groups, const void *arguments) override {
        if (!commandList || groups.x == 0 || groups.y == 0 || groups.z == 0) return;
        nvrhi::ComputeState state;
        state.setPipeline(m_pipeline);
        if (m_bindingSet) state.addBindingSet(m_bindingSet);
        commandList->setComputeState(state);
        if (m_argumentsSize) commandList->setPushConstants(arguments, m_argumentsSize);
        commandList->dispatch(groups.x, groups.y, groups.z);
    }

 private:
    std::string m_name;
    nvrhi::ShaderHandle m_shader;   // keeps the reflection alive
    const nvrhi::ShaderEntryPointReflection *m_entryPoint = nullptr;
    const nvrhi::ShaderLibraryReflection *m_library = nullptr;
    uint32_t m_argumentsSize = 0;
    nvrhi::BindingLayoutHandle m_layout;
    nvrhi::BindingSetHandle m_bindingSet;
    nvrhi::ComputePipelineHandle m_pipeline;
};

class ComputeContext final : public nvrhi::ObjectImpl<IComputeContext> {
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ComputeContext)
    NVRHI_IMPLEMENTS_INTERFACE(IComputeContext)
    NVRHI_END_INTERFACE_TABLE()

    ComputeContext(nvrhi::cuda::IDevice *device, vfs::IFileSystem *fs) : m_device(device), m_fs(fs) {}

    ~ComputeContext() {
        // The recorded work may use objects that are released with the context's users.
        finish();
    }

    bool initialize() {
        if (NVRHI_FAILED(m_device->createCommandList(nvrhi::CommandListParameters(), &m_commandList)) ||
            NVRHI_FAILED(m_device->createEventQuery(&m_event))) {
            log::error("GVDB: cannot create the command list of the compute context");
            return false;
        }
        return true;
    }

    nvrhi::cuda::IDevice *getDevice() const override { return m_device.Get(); }
    vfs::IFileSystem *getFileSystem() const override { return m_fs.Get(); }

    nvrhi::ICommandList *getCommandList() override {
        if (!m_open) {
            m_commandList->open();
            m_open = true;
        }
        return m_commandList.Get();
    }

    void flush() override {
        if (m_open) {
            m_commandList->close();
            m_open = false;
            const uint64_t instance = m_device->executeCommandList(m_commandList);
            for (auto &object : m_pendingRelease) m_inFlightRelease.emplace_back(instance, std::move(object));
            m_pendingRelease.clear();
        }
        // Objects of finished submissions go.
        const uint64_t completed = m_device->queueGetCompletedInstance(nvrhi::CommandQueue::Graphics);
        m_inFlightRelease.erase(std::remove_if(m_inFlightRelease.begin(), m_inFlightRelease.end(),
                                               [&](const auto &entry) { return entry.first <= completed; }),
                                m_inFlightRelease.end());
    }

    void finish() override {
        flush();
        m_device->resetEventQuery(m_event);
        m_device->setEventQuery(m_event, nvrhi::CommandQueue::Graphics);
        m_device->waitEventQuery(m_event);
        m_inFlightRelease.clear();
        m_pendingRelease.clear();   // nothing was recorded since the flush
    }

    void releaseAfterUse(nvrhi::IObject *object) override {
        if (object) m_pendingRelease.emplace_back(object);
    }

    void readBuffer(nvrhi::IBuffer *buffer, uint64_t offset, void *data, uint64_t size) override {
        if (!buffer || !data || size == 0) return;
        if (!m_readback || m_readback->getDesc().byteSize < size) {
            nvrhi::BufferDesc desc;
            desc.byteSize = std::max<uint64_t>(size, 64 * 1024);
            desc.cpuAccess = nvrhi::CpuAccessMode::Read;
            desc.debugName = "GVDB readback";
            m_readback = nullptr;
            if (NVRHI_FAILED(m_device->createBuffer(desc, &m_readback))) {
                log::error("GVDB: cannot create a readback buffer of %llu bytes", (unsigned long long)size);
                return;
            }
        }
        getCommandList()->copyBuffer(m_readback, 0, buffer, offset, size);
        finish();
        const void *mapped = m_device->mapBuffer(m_readback, nvrhi::CpuAccessMode::Read);
        if (mapped) memcpy(data, mapped, size_t(size));
        m_device->unmapBuffer(m_readback);
    }

    IComputeKernel *findKernel(const char *const *containerPaths, uint32_t count, const char *entryName) override {
        if (!containerPaths || !entryName) return nullptr;
        for (uint32_t i = 0; i < count; ++i) {
            nvrhi::IShaderLibrary *library = getLibrary(containerPaths[i]);
            if (library && nvrhi::findShaderEntryPoint(library->getReflection(), entryName))
                return getKernel(containerPaths[i], entryName);
        }
        log::error("GVDB: no shader container defines the kernel %s", entryName);
        return nullptr;
    }

    IComputeKernel *getKernel(const char *containerPath, const char *entryName) override {
        if (!containerPath || !entryName) return nullptr;
        const std::string key = std::string(containerPath) + '\n' + entryName;
        auto it = m_kernels.find(key);
        if (it != m_kernels.end()) return it->second.Get();   // null when it failed before

        nvrhi::AutoPtr<ComputeKernel> kernel;
        nvrhi::IShaderLibrary *library = getLibrary(containerPath);
        nvrhi::ShaderHandle shader;
        if (library && NVRHI_SUCCEEDED(library->getShader(entryName, nvrhi::ShaderType::Compute, &shader)) && shader) {
            kernel = MAKE_RC_OBJ_PTR(ComputeKernel);
            if (!kernel->initialize(m_device, shader, entryName)) kernel = nullptr;
        } else if (library) {
            log::error("GVDB: %s has no kernel %s", containerPath, entryName);
        }
        IComputeKernel *raw = kernel.Get();
        m_kernels.emplace(key, std::move(kernel));
        return raw;
    }

 private:
    nvrhi::IShaderLibrary *getLibrary(const char *path) {
        auto it = m_libraries.find(path);
        if (it != m_libraries.end()) return it->second.Get();
        nvrhi::ShaderLibraryHandle library;
        nvrhi::AutoPtr<nvrhi::IDataBlob> blob;
        if (!m_fs || NVRHI_FAILED(m_fs->readFile(path, &blob)) || !blob) {
            log::error("GVDB: cannot read the shader container %s", path);
        } else if (NVRHI_FAILED(m_device->createShaderLibrary(blob->GetDataPtr(), blob->GetSize(), &library))) {
            log::error("GVDB: cannot load the shader container %s", path);
            library = nullptr;
        }
        nvrhi::IShaderLibrary *raw = library.Get();
        m_libraries.emplace(path, std::move(library));
        return raw;
    }

    nvrhi::AutoPtr<nvrhi::cuda::IDevice> m_device;
    nvrhi::AutoPtr<vfs::IFileSystem> m_fs;
    nvrhi::CommandListHandle m_commandList;
    bool m_open = false;
    nvrhi::EventQueryHandle m_event;
    nvrhi::BufferHandle m_readback;
    std::vector<nvrhi::AutoPtr<nvrhi::IObject>> m_pendingRelease;   // used by recorded work
    std::vector<std::pair<uint64_t, nvrhi::AutoPtr<nvrhi::IObject>>> m_inFlightRelease;   // by submitted work
    std::map<std::string, nvrhi::ShaderLibraryHandle> m_libraries;
    std::map<std::string, nvrhi::AutoPtr<ComputeKernel>> m_kernels;
};

}  // namespace

nvrhi::FRESULT createComputeContext(nvrhi::cuda::IDevice *device, vfs::IFileSystem *fs, IComputeContext **context) {
    if (!device || !context) return nvrhi::FE_INVALID_ARGS;
    *context = nullptr;
    nvrhi::AutoPtr<ComputeContext> object = MAKE_RC_OBJ_PTR(ComputeContext, device, fs);
    if (!object) return nvrhi::FE_OUT_OF_MEMORY;
    if (!object->initialize()) return nvrhi::FE_GENERIC_ERROR;
    *context = object.Detach();
    return nvrhi::FS_OK;
}

dm::uint3 groupsFor(IComputeKernel *kernel, dm::uint3 threads) {
    if (!kernel) return dm::uint3(0u);
    const dm::uint3 size = kernel->getThreadGroupSize();
    return dm::uint3(dm::div_ceil(threads.x, size.x), dm::div_ceil(threads.y, size.y), dm::div_ceil(threads.z, size.z));
}

namespace detail {

bool checkKernelArguments(IComputeKernel *kernel, uint32_t count) {
    if (!kernel) return false;   // getKernel() reported why
    if (kernel->getArgumentCount() != count) {
        log::error("GVDB: kernel %s takes %u arguments, %u were given", kernel->getName(), kernel->getArgumentCount(),
                   count);
        NVRHI_ASSERT(0);
        return false;
    }
    return true;
}

bool writeKernelArgument(IComputeKernel *kernel, uint32_t index, uint8_t *block, const void *value, size_t size) {
    const nvrhi::ShaderParameterReflection *argument = kernel->getArgument(index);
    if (!argument || argument->uniformSize != size ||
        size_t(argument->uniformOffset) + size > kernel->getArgumentsSize()) {
        log::error("GVDB: argument %u (%s) of kernel %s is %u bytes, %zu were given", index,
                   argument ? argument->name.c_str() : "?", kernel->getName(), argument ? argument->uniformSize : 0u,
                   size);
        NVRHI_ASSERT(0);
        return false;
    }
    memcpy(block + argument->uniformOffset, value, size);
    return true;
}

}  // namespace detail

}  // namespace gvdb

// SPDX-License-Identifier: MIT
#include <atomic>
#include <cstdio>
#include <cstring>
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <fstream>
#include <nvrhi/core/foundation.h>
#include <nvrhi/cuda.h>
#include <nvrhi/graphics-interop.h>
#include <nvrhi/shader-program.h>
#include <stdexcept>
#include <vector>
using namespace nvrhi;
namespace
{
std::atomic_uint validationProblems{0};
void require(bool value, const char *text)
{
    if (!value)
        throw std::runtime_error(text);
}
void check(FRESULT code, const char *text)
{
    if (NVRHI_FAILED(code))
        throw std::runtime_error(std::string(text) + ": " + std::to_string(code));
}
void submit(IDevice *device, ICommandList *commands)
{
    commands->close();
    const auto serial = device->executeCommandList(commands);
    require(serial != 0 || device->getGraphicsAPI() == GraphicsAPI::D3D11, "GPU submission");
}
struct Messages : ObjectImpl<IMessageCallback>
{
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity severity, const char *text) noexcept override
    {
        if (severity >= MessageSeverity::Warning) ++validationProblems;
        std::fprintf(stderr, "CUDA: %s\n", text);
    }
};
int run(IDevice *graphics)
{
    auto callback = MAKE_RC_OBJ_PTR(Messages);
    cuda::DeviceDesc cudaDesc;
    cudaDesc.enableOptiX = false;
    cudaDesc.errorCB = callback;
    auto cudaDevice = cuda::createDevice(cudaDesc);
    if (!cudaDevice)
        return 77;
    InteropDeviceHandle importer;
    check(cuda::nvrhiCudaCreateInteropDevice(cudaDevice, &importer), "create CUDA importer");
    GraphicsInteropDeviceHandle exporter;
    check(nvrhiCreateGraphicsInteropDevice(graphics, &exporter), "create unified graphics exporter");
    InteropRouteCapabilities route;
    check(exporter->queryRoute(GraphicsAPI::CUDA, importer->getAdapterIdentity(), route), "query graphics/CUDA route");
    require(route.sameAdapter && route.textureSharing && route.timelineFences, "texture and GPU timeline route");
    auto wrong = importer->getAdapterIdentity();
    wrong.uuid[0] ^= 1;
    wrong.luid[0] ^= 1;
    require(exporter->queryRoute(GraphicsAPI::CUDA, wrong, route) == FE_UNSUPPORTED, "adapter mismatch rejected");
    require(exporter->queryRoute(GraphicsAPI::D3D12, importer->getAdapterIdentity(), route) == FE_UNSUPPORTED,
            "unsupported graphics-to-graphics route rejected");
    check(exporter->queryRoute(GraphicsAPI::CUDA, importer->getAdapterIdentity(), route), "restore route");
    TextureDesc td;
    td.width = 65;
    td.height = 33;
    const uint32_t count = td.width * td.height;
    td.format = Format::R32_UINT;
    td.isUAV = true;
    td.sharedResourceFlags = route.textureSharingFlags;
    td.initialState = ResourceStates::Common;
    td.keepInitialState = false;
    TextureHandle texture;
    check(graphics->createTexture(td, &texture), "create shared texture");
    ExternalMemoryHandle memory;
    check(exporter->exportTexture(texture, &memory), "export texture");
    TextureHandle cudaTexture;
    check(importer->importTexture(memory->getDesc(), td, &cudaTexture), "import texture");
    auto unshared = td;
    unshared.sharedResourceFlags = SharedResourceFlags::None;
    TextureHandle ordinary;
    check(graphics->createTexture(unshared, &ordinary), "create ordinary texture");
    ExternalMemoryHandle rejected;
    require(exporter->exportTexture(ordinary, &rejected) == FE_UNSUPPORTED && !rejected, "ordinary resource rejected");
    const auto foreignStatus = exporter->exportTexture(cudaTexture, &rejected);
    require((foreignStatus == FE_INVALID_ARGS || foreignStatus == FE_UNSUPPORTED) && !rejected,
            "foreign API resource rejected");
    require(exporter->exportTexture(texture, nullptr) == FE_INVALID_ARGS, "null export result rejected");
    if (graphics->getGraphicsAPI() == GraphicsAPI::D3D11)
    {
        auto legacy = td;
        legacy.sharedResourceFlags = SharedResourceFlags::Shared;
        TextureHandle kmt;
        check(graphics->createTexture(legacy, &kmt), "create legacy KMT texture");
        require(exporter->exportTexture(kmt, &rejected) == FE_UNSUPPORTED && !rejected,
                "unsafe KMT export route rejected");
    }
    BufferDesc bd;
    bd.byteSize = count * sizeof(uint32_t);
    bd.structStride = sizeof(uint32_t);
    bd.canHaveUAVs = true;
    bd.sharedResourceFlags = SharedResourceFlags::Shared;
    bd.initialState = ResourceStates::Common;
    BufferHandle buffer, cudaBuffer;
    ExternalMemoryHandle bufferMemory;
    if (route.bufferSharing)
    {
        check(graphics->createBuffer(bd, &buffer), "create shared buffer");
        check(exporter->exportBuffer(buffer, &bufferMemory), "export buffer");
        check(importer->importBuffer(bufferMemory->getDesc(), bd, &cudaBuffer), "import buffer");
    }
    else
    {
        auto local = bd;
        local.sharedResourceFlags = SharedResourceFlags::None;
        check(cudaDevice->createBuffer(local, &cudaBuffer), "create CUDA-only buffer for D3D11 route");
        const auto unsupported = exporter->exportBuffer(cudaBuffer, &rejected);
        require((unsupported == FE_UNSUPPORTED || unsupported == FE_INVALID_ARGS) && !rejected,
                "D3D11 buffer route rejected");
    }
    ExternalFenceHandle fence, cudaFence;
    check(exporter->createFence(0, &fence), "create shared timeline fence");
    check(importer->importFence(fence->getDesc(), &cudaFence), "import timeline fence");
    CommandListHandle graphicsCommands, cudaCommands;
    check(graphics->createCommandList(CommandListParameters(), &graphicsCommands), "graphics commands");
    check(cudaDevice->createCommandList(CommandListParameters(), &cudaCommands), "CUDA commands");
    std::vector<uint32_t> pixels(count), values(count, 11);
    for (uint32_t i = 0; i < count; ++i)
        pixels[i] = i;
    graphicsCommands->open();
    graphicsCommands->beginTrackingTextureState(texture, AllSubresources, ResourceStates::Common);
    graphicsCommands->writeTexture(texture, 0, 0, pixels.data(), td.width * sizeof(uint32_t));
    if (buffer)
    {
        graphicsCommands->beginTrackingBufferState(buffer, ResourceStates::Common);
        graphicsCommands->writeBuffer(buffer, values.data(), values.size() * sizeof(uint32_t));
        check(exporter->releaseResource(graphicsCommands, bufferMemory), "release graphics buffer");
    }
    check(exporter->releaseResource(graphicsCommands, memory), "release graphics texture");
    submit(graphics, graphicsCommands);
    check(exporter->signalFence(CommandQueue::Graphics, fence, 1), "graphics producer signal");
    const auto shaderPath =
        donut::app::GetDirectoryWithExecutable() / "shaders/interop/ptx/InteropTexture_updateShared.bin";
    std::ifstream shaderFile(shaderPath, std::ios::binary);
    require(bool(shaderFile), "open Slang interoperability artifact");
    const std::vector<char> shaderBytes((std::istreambuf_iterator<char>(shaderFile)), std::istreambuf_iterator<char>());
    ShaderProgramHandle program;
    check(createShaderProgram(cudaDevice, shaderBytes.data(), shaderBytes.size(), &program), "load Slang program");
    ShaderHandle shader;
    check(program->createShader(0, &shader), "create Slang compute shader");
    BindingLayoutDesc layoutDesc;
    check(program->getBindingLayoutDesc(0, 0, layoutDesc), "reflect Slang bindings");
    BindingLayoutHandle layout;
    check(cudaDevice->createBindingLayout(layoutDesc, &layout), "create binding layout");
    BindingSetDesc bindingDesc;
    bindingDesc.addItem(BindingSetItem::Texture_UAV(0, cudaTexture));
    bindingDesc.addItem(BindingSetItem::StructuredBuffer_UAV(1, cudaBuffer));
    BindingSetHandle bindings;
    check(cudaDevice->createBindingSet(bindingDesc, layout, &bindings), "create shared binding set");
    ComputePipelineHandle pipeline;
    check(cudaDevice->createComputePipeline(ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout),
                                            &pipeline),
          "create shared compute pipeline");
    if (!buffer)
    {
        cudaCommands->open();
        cudaCommands->writeBuffer(cudaBuffer, values.data(), values.size() * sizeof(uint32_t));
        submit(cudaDevice, cudaCommands);
    }
    // Repeated ownership and timeline cycles exercise retained imports and both directions.
    for (uint64_t cycle = 0; cycle < 3; ++cycle)
    {
        cudaCommands->open();
        check(importer->waitFence(cudaCommands, cudaFence, cycle * 2 + 1), "CUDA wait graphics");
        cudaCommands->setComputeState(ComputeState().setPipeline(pipeline).addBindingSet(bindings));
        cudaCommands->dispatch((td.width + 7) / 8, (td.height + 3) / 4, 1);
        check(importer->signalFence(cudaCommands, cudaFence, cycle * 2 + 2), "CUDA consumer signal");
        submit(cudaDevice, cudaCommands);
        check(exporter->waitFence(CommandQueue::Graphics, fence, cycle * 2 + 2), "graphics wait CUDA");
        graphicsCommands->open();
        check(exporter->acquireResource(graphicsCommands, memory, ResourceStates::CopySource),
              "acquire graphics texture");
        if (bufferMemory)
            check(exporter->acquireResource(graphicsCommands, bufferMemory, ResourceStates::CopySource),
                  "acquire graphics buffer");
        if (cycle < 2)
        {
            check(exporter->releaseResource(graphicsCommands, memory), "release next texture cycle");
            if (bufferMemory)
                check(exporter->releaseResource(graphicsCommands, bufferMemory), "release next buffer cycle");
        }
        submit(graphics, graphicsCommands);
        check(exporter->signalFence(CommandQueue::Graphics, fence, cycle * 2 + 3), "graphics cycle signal");
    }
    // Readback is test verification only; graphics/CUDA transfers above share allocations.
    StagingTextureHandle staging;
    auto stagingDesc = td;
    stagingDesc.sharedResourceFlags = SharedResourceFlags::None;
    stagingDesc.isUAV = false;
    check(graphics->createStagingTexture(stagingDesc, CpuAccessMode::Read, &staging), "create verification staging");
    graphicsCommands->open();
    graphicsCommands->beginTrackingTextureState(texture, AllSubresources, ResourceStates::CopySource);
    graphicsCommands->copyTexture2(staging, TextureSlice(), texture, TextureSlice());
    submit(graphics, graphicsCommands);
    graphics->waitForIdle();
    cudaDevice->waitForIdle();
    size_t rowPitch = 0;
    const auto mapped = static_cast<const uint8_t *>(
        graphics->mapStagingTexture(staging, TextureSlice(), CpuAccessMode::Read, rowPitch));
    require(mapped != nullptr, "map graphics verification texture");
    for (uint32_t y = 0; y < td.height; ++y)
        for (uint32_t x = 0; x < td.width; ++x)
            require(reinterpret_cast<const uint32_t *>(mapped + y * rowPitch)[x] == y * td.width + x + 36,
                    "graphics observes CUDA shared texture updates");
    graphics->unmapStagingTexture(staging);
    if (buffer)
    {
        auto readDesc = bd;
        readDesc.sharedResourceFlags = SharedResourceFlags::None;
        readDesc.cpuAccess = CpuAccessMode::Read;
        readDesc.canHaveUAVs = false;
        readDesc.initialState = ResourceStates::CopyDest;
        readDesc.keepInitialState = true;
        BufferHandle readback;
        check(graphics->createBuffer(readDesc, &readback), "create verification buffer");
        graphicsCommands->open();
        graphicsCommands->beginTrackingBufferState(buffer, ResourceStates::CopySource);
        graphicsCommands->copyBuffer(readback, 0, buffer, 0, bd.byteSize);
        submit(graphics, graphicsCommands);
        graphics->waitForIdle();
        auto data = static_cast<const uint32_t *>(graphics->mapBuffer(readback, CpuAccessMode::Read));
        require(data != nullptr, "map verification buffer");
        for (unsigned i = 0; i < count; ++i)
            require(data[i] == 14, "graphics observes CUDA shared buffer updates");
        graphics->unmapBuffer(readback);
    }
    // Imports retain their exported allocation/fence owners after caller handles drop.
    memory = nullptr;
    bufferMemory = nullptr;
    texture = nullptr;
    buffer = nullptr;
    fence = nullptr;
    exporter = nullptr;
    cudaCommands->open();
    check(importer->waitFence(cudaCommands, cudaFence, 7), "retained fence lifetime");
    submit(cudaDevice, cudaCommands);
    cudaDevice->waitForIdle();
    std::puts("PASS unified graphics/CUDA export, native texture/buffer sharing, three timeline/ownership cycles and "
              "lifetimes");
    return 0;
}
} // namespace
int main(int argc, const char **argv)
{
    donut::log::ConsoleApplicationMode();
    donut::log::SetCallback([](donut::log::Severity severity, const char* text) {
        if (severity >= donut::log::Severity::Warning) ++validationProblems;
        std::fprintf(stderr, "%s\n", text);
    });
    try
    {
        const auto api = donut::app::GetGraphicsAPIFromCommandLine(argc, argv);
        auto manager = TakeOver(donut::app::DeviceManager::Create(api));
        require(bool(manager), "graphics backend unavailable");
        donut::app::DeviceCreationParameters params;
        params.enableNvrhiValidationLayer = true;
        for (int i = 1; i < argc; ++i)
            if (!std::strcmp(argv[i], "--validation"))
                params.enableDebugRuntime = true;
        if (api == GraphicsAPI::VULKAN)
        {
#ifdef _WIN32
            params.requiredVulkanDeviceExtensions = {"VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32"};
#else
            params.requiredVulkanDeviceExtensions = {"VK_KHR_external_memory_fd", "VK_KHR_external_semaphore_fd"};
#endif
        }
        require(manager->CreateHeadlessDevice(params), "graphics device creation");
        const auto code = run(manager->GetDevice());
        manager->Shutdown();
        require(validationProblems == 0, "native/NVRHI validation or teardown warnings");
        return code;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}

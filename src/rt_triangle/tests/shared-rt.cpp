// SPDX-License-Identifier: MIT
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <nvrhi/shader-program.h>
#if DONUT_WITH_DX12
#include <d3d12.h>
#include <d3d12sdklayers.h>
#endif
#if ETHEREAL_TEST_CUDA
#include <nvrhi/cuda.h>
#endif
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
using namespace nvrhi;
namespace
{
std::atomic<unsigned> errors{0};
void require(bool value, const char* text)
{
    if (!value) throw std::runtime_error(text);
}
#define OK(c) require(NVRHI_SUCCEEDED(c), #c)
struct Messages : ObjectImpl<IMessageCallback>
{
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity severity, const char* text) noexcept override
    {
        if (severity >= MessageSeverity::Error) ++errors;
        std::fprintf(stderr, "NVRHI: %s\n", text);
    }
};
void reportNativeErrors(IDevice* device)
{
#if DONUT_WITH_DX12
    if (device->getGraphicsAPI() != GraphicsAPI::D3D12) return;
    auto* native = static_cast<ID3D12Device*>(device->getNativeObject(ObjectTypes::D3D12_Device));
    ID3D12InfoQueue* queue = nullptr;
    if (!native || FAILED(native->QueryInterface(IID_PPV_ARGS(&queue)))) return;
    for (UINT64 i = 0; i < queue->GetNumStoredMessages(); ++i)
    {
        SIZE_T size = 0;
        queue->GetMessage(i, nullptr, &size);
        std::vector<uint8_t> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        if (SUCCEEDED(queue->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
        {
            ++errors;
            std::fprintf(stderr, "D3D12: %s\n", message->pDescription);
        }
    }
    queue->ClearStoredMessages();
    queue->Release();
#endif
}
ShaderHandle shader(IShaderProgram* program, const char* name, ShaderType stage)
{
    uint32_t index = ~0u;
    OK(program->findEntryPoint(name, stage, &index));
    ShaderHandle result;
    OK(program->createShader(index, &result));
    return result;
}
int run(IDevice* device, const char* path)
{
    if (!device->queryFeatureSupport(Feature::RayTracingPipeline))
    {
        std::puts("SKIP: adapter lacks ray tracing pipelines");
        return 77;
    }
    std::ifstream input(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    ShaderProgramHandle program;
    OK(nvrhiCreateShaderProgram(device, bytes.data(), bytes.size(), &program));
    uint32_t raygenIndex = ~0u;
    OK(program->findEntryPoint("rayGeneration", ShaderType::RayGeneration, &raygenIndex));
    ShaderEntryPointDesc raygenDesc;
    OK(program->getEntryPointDesc(raygenIndex, raygenDesc));
    if (device->getGraphicsAPI() != GraphicsAPI::D3D12)
        require(raygenDesc.entryParamsSize == 40, "five-pointer physical shader-record payload");
    BindingLayoutDesc reflectedGlobals;
    OK(program->getBindingLayoutDesc(raygenIndex, 0, reflectedGlobals));
    require(reflectedGlobals.bindings.size() == 2, "local shader records must be excluded from global layouts");
    const auto raygen = shader(program, "rayGeneration", ShaderType::RayGeneration);
    const auto miss = shader(program, "rayMiss", ShaderType::Miss);
    const auto hit = shader(program, "rayHit", ShaderType::ClosestHit);
    const auto intersection = shader(program, "rayIntersection", ShaderType::Intersection);
    BindingLayoutHandle globals, local, raygenLocal;
    OK(device->createBindingLayout(BindingLayoutDesc().setVisibility(ShaderType::AllRayTracing)
        .addItem(BindingLayoutItem::RayTracingAccelStruct(0))
        .addItem(BindingLayoutItem::StructuredBuffer_UAV(0)), &globals));
    OK(device->createBindingLayout(BindingLayoutDesc().setVisibility(ShaderType::AllRayTracing)
        .addItem(BindingLayoutItem::ConstantBuffer(0)), &local));
    BindingLayoutDesc raygenLayout;
    raygenLayout.visibility = ShaderType::AllRayTracing;
    for (unsigned i = 0; i < 5; ++i) raygenLayout.addItem(BindingLayoutItem::ConstantBuffer(i));
    OK(device->createBindingLayout(raygenLayout, &raygenLocal));
    rt::PipelineHandle pipeline;
    const auto pipelineResult = device->createRayTracingPipeline(rt::PipelineDesc()
        .addShader(rt::PipelineShaderDesc().setExportName("raygen").setShader(raygen).setBindingLayout(raygenLocal))
        .addShader(rt::PipelineShaderDesc().setExportName("miss").setShader(miss).setBindingLayout(local))
        .addHitGroup(rt::PipelineHitGroupDesc().setExportName("triangle").setClosestHitShader(hit).setBindingLayout(local))
        .addHitGroup(rt::PipelineHitGroupDesc().setExportName("procedural").setClosestHitShader(hit)
            .setIntersectionShader(intersection).setIsProceduralPrimitive(true).setBindingLayout(local))
        .addBindingLayout(globals).setMaxPayloadSize(4).setMaxAttributeSize(8), &pipeline);
    reportNativeErrors(device);
    OK(pipelineResult);
    BufferHandle vertices, boxes, output, readback;
    const float vertexData[] = {-0.75f,-0.75f,0, 0.75f,-0.75f,0, 0,0.75f,0};
    const rt::GeometryAABB box{-0.5f,-0.5f,0, 0.5f,0.5f,1};
    OK(device->createBuffer(BufferDesc().enableAutomaticStateTracking(ResourceStates::Common).setByteSize(sizeof(vertexData)).setIsAccelStructBuildInput(true), &vertices));
    OK(device->createBuffer(BufferDesc().enableAutomaticStateTracking(ResourceStates::Common).setByteSize(sizeof(box)).setIsAccelStructBuildInput(true), &boxes));
    OK(device->createBuffer(BufferDesc().enableAutomaticStateTracking(ResourceStates::Common).setByteSize(16).setStructStride(4).setCanHaveUAVs(true), &output));
    OK(device->createBuffer(BufferDesc().enableAutomaticStateTracking(ResourceStates::Common).setByteSize(32).setCpuAccess(CpuAccessMode::Read), &readback));
    rt::GeometryDesc triangle, procedural;
    triangle.setTriangles(rt::GeometryTriangles().setVertexBuffer(vertices).setVertexCount(3)
        .setVertexFormat(Format::RGB32_FLOAT).setVertexStride(12)).setFlags(rt::GeometryFlags::Opaque);
    procedural.setAABBs(rt::GeometryAABBs().setBuffer(boxes).setCount(1).setStride(sizeof(box)))
        .setFlags(rt::GeometryFlags::Opaque);
    rt::AccelStructHandle triangleBlas, proceduralBlas, tlas;
    OK(device->createAccelStruct(rt::AccelStructDesc().addBottomLevelGeometry(triangle), &triangleBlas));
    OK(device->createAccelStruct(rt::AccelStructDesc().addBottomLevelGeometry(procedural), &proceduralBlas));
    OK(device->createAccelStruct(rt::AccelStructDesc().setTopLevelMaxInstances(3), &tlas));
    rt::InstanceDesc instances[3];
    for (unsigned i = 0; i < 3; ++i)
    {
        instances[i].setBLAS(i == 1 ? proceduralBlas : triangleBlas).setInstanceMask(255);
        instances[i].instanceContributionToHitGroupIndex = i;
        instances[i].transform[3] = float(i * 2) - 2;
    }
    BindingSetHandle globalSet;
    OK(device->createBindingSet(BindingSetDesc()
        .addItem(BindingSetItem::RayTracingAccelStruct(0, tlas))
        .addItem(BindingSetItem::StructuredBuffer_UAV(0, output)), globals, &globalSet));
    CommandListHandle commands;
    OK(device->createCommandList(CommandListParameters(), &commands));
    commands->open();
    commands->writeBuffer(vertices, vertexData, sizeof(vertexData));
    commands->writeBuffer(boxes, &box, sizeof(box));
    commands->buildBottomLevelAccelStruct(triangleBlas, &triangle, 1);
    commands->buildBottomLevelAccelStruct(proceduralBlas, &procedural, 1);
    commands->buildTopLevelAccelStruct(tlas, instances, 3);
    std::vector<BindingSetHandle> materials;
    auto makeLocal = [&](const void* data, size_t size)
    {
        // DXR root CBVs require 256-byte aligned storage. Each API shares this allocation.
        BufferHandle buffer;
        OK(device->createBuffer(BufferDesc().enableAutomaticStateTracking(ResourceStates::Common).setByteSize(256).setIsConstantBuffer(true), &buffer));
        uint8_t contents[256]{};
        std::memcpy(contents, data, size);
        commands->writeBuffer(buffer, contents, sizeof(contents));
        commands->setPermanentBufferState(buffer, ResourceStates::ConstantBuffer);
        BindingSetHandle set;
        OK(device->createBindingSet(BindingSetDesc().addItem(BindingSetItem::ConstantBuffer(0, buffer)), local, &set));
        return set;
    };
    const uint32_t expected[] = {11, 22, 33, 7};
    for (uint32_t value : expected) materials.push_back(makeLocal(&value, sizeof(value)));
    const float settings[] = {-2, 2, 0, 0};
    auto settingsBufferSet = makeLocal(settings, sizeof(settings));
    const uint32_t bias = 0;
    auto biasBufferSet = makeLocal(&bias, sizeof(bias));
    BindingSetDesc settingsDesc;
    settingsDesc.addItem(settingsBufferSet->getDesc()->bindings[0]);
    for (unsigned i = 1; i < 5; ++i)
    {
        auto item = biasBufferSet->getDesc()->bindings[0];
        item.slot = i;
        settingsDesc.addItem(item);
    }
    BindingSetHandle settingsSet;
    OK(device->createBindingSet(settingsDesc, raygenLocal, &settingsSet));
    settingsBufferSet = nullptr;
    biasBufferSet = nullptr;
    // Finish uploads first so their command list cannot mask missing dispatch
    // retention of shader-record material buffers.
    commands->close();
    require(device->executeCommandList(commands) != 0, "upload submission");
    device->waitForIdle();
    device->runGarbageCollection();
    commands->open();
    std::vector<rt::ShaderTableHandle> tables;
    for (unsigned cached = 0; cached < 2; ++cached)
    {
        rt::ShaderTableHandle table;
        OK(pipeline->createShaderTable(rt::ShaderTableDesc().setIsCached(cached != 0).setMaxEntries(5), &table));
        table->setRayGenerationShader("raygen", settingsSet);
        require(table->addMissShader("miss", materials[3]) == 0, "miss record");
        require(table->addHitGroup("triangle", materials[0]) == 0, "first triangle record");
        require(table->addHitGroup("procedural", materials[1]) == 1, "procedural record");
        require(table->addHitGroup("triangle", materials[2]) == 2, "second triangle record");
        commands->setRayTracingState(rt::State().setShaderTable(table).addBindingSet(globalSet));
        commands->dispatchRays(rt::DispatchRaysArguments().setWidth(4));
        commands->copyBuffer(readback, cached * 16, output, 0, 16);
        tables.push_back(table);
    }
    commands->close();
    // Clearing the table after recording must preserve both uploaded record data
    // and the buffer ownership captured by each command list.
    for (const auto& table : tables)
    {
        table->clearHitShaders();
        table->clearMissShaders();
    }
    materials.clear();
    settingsSet = nullptr;
    require(device->executeCommandList(commands) != 0, "command submission");
    device->waitForIdle();
    const auto* result = static_cast<const uint32_t*>(device->mapBuffer(readback, CpuAccessMode::Read));
    require(result != nullptr, "readback mapping");
    bool matches = true;
    for (unsigned i = 0; i < 8; ++i)
        if (result[i] != expected[i % 4])
        {
            std::fprintf(stderr, "record %u: expected %u, actual %u\n", i, expected[i % 4], result[i]);
            matches = false;
        }
    device->unmapBuffer(readback);
    reportNativeErrors(device);
    require(matches, "shared ray tracing material readback");
    std::puts("PASS triangles + procedural AABB, per-instance materials, raygen/miss/hit shader records, cached/uncached SBT and recorded-resource lifetime");
    return 0;
}
}
int main(int argc, char** argv)
{
    if (argc < 3) return 1;
    donut::log::ConsoleApplicationMode();
    donut::log::SetCallback([](donut::log::Severity severity, const char* text) {
        if (severity >= donut::log::Severity::Error) ++errors;
        std::fprintf(stderr, "%s\n", text);
    });
    try
    {
#if ETHEREAL_TEST_CUDA
        if (std::strcmp(argv[1], "-cuda") == 0)
        {
            auto callback = MAKE_RC_OBJ_PTR(Messages);
            cuda::DeviceDesc desc;
            desc.errorCB = callback;
            auto device = cuda::createDevice(desc);
            require(device != nullptr, "CUDA device creation");
            const int result = run(device, argv[2]);
            return errors ? 1 : result;
        }
#endif
        const auto api = std::strcmp(argv[1], "-vk") == 0 ? GraphicsAPI::VULKAN : GraphicsAPI::D3D12;
        auto manager = TakeOver(donut::app::DeviceManager::Create(api));
        require(manager != nullptr, "graphics backend unavailable");
        donut::app::DeviceCreationParameters params;
        params.enableNvrhiValidationLayer = true;
        params.enableRayTracingExtensions = true;
        params.enableDebugRuntime = argc > 3 && std::strcmp(argv[3], "--native-validation") == 0;
        require(manager->CreateHeadlessDevice(params), "graphics device creation");
        const int result = run(manager->GetDevice(), argv[2]);
        manager->Shutdown();
        return errors ? 1 : result;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}

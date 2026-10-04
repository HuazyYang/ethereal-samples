// nvrhi/d3d12.h brings in <directx/d3d12.h>; it must precede nvapi.h (included by NvMeshShaderPsoExt.h).
#include <nvrhi/d3d12.h>

#include "meshlets/NvMeshShaderApi.h"
#include "meshlets/NvMeshShaderPsoExt.h"

#include <donut/core/log.h>

using namespace donut;

#ifdef ASTEROIDS_WITH_NVAPI

#include <Windows.h>

namespace
{
    constexpr unsigned int c_NvApiQueryMeshShaderSupport = 0xA47716F8;   // 0x140001980
    constexpr unsigned int c_NvApiDispatchMeshTasks = 0x8AB10C89;        // 0x140001AD0

    // nvapi_QueryInterface resolves NVAPI entry points by interface id. The 2018 static nvapi64.lib called it
    // through its own pointer (qword_1402E02F8) to nvapi64.dll's export; the current nvapi64.lib no longer
    // exports that symbol, so the export is looked up directly (NvAPI_Initialize has loaded the DLL).
    using PFN_NvApiQueryInterface = void* (__cdecl*)(unsigned int interfaceId);

    void* nvapi_QueryInterface(unsigned int interfaceId)
    {
        static PFN_NvApiQueryInterface s_QueryInterface = []() -> PFN_NvApiQueryInterface
        {
            HMODULE module = GetModuleHandleA("nvapi64.dll");
            if (!module)
                module = LoadLibraryA("nvapi64.dll");
            return module ? PFN_NvApiQueryInterface(GetProcAddress(module, "nvapi_QueryInterface")) : nullptr;
        }();
        return s_QueryInterface ? s_QueryInterface(interfaceId) : nullptr;
    }

    // The second parameter is a 1-byte flag (WinMain passes the address of a zeroed char and tests it for non-zero).
    using PFN_QueryMeshShaderSupport = NvAPI_Status(__cdecl*)(ID3D12Device* device, NvU8* supported);
    using PFN_DispatchMeshTasks = NvAPI_Status(__cdecl*)(ID3D12GraphicsCommandList* commandList, NvU32 numTasks);

    PFN_DispatchMeshTasks GetDispatchMeshTasks()
    {
        static PFN_DispatchMeshTasks s_Function = PFN_DispatchMeshTasks(nvapi_QueryInterface(c_NvApiDispatchMeshTasks));
        return s_Function;
    }

    D3D12_RENDER_TARGET_BLEND_DESC TranslateTargetBlend(const nvrhi::BlendState::RenderTarget& target)
    {
        // nvrhi's BlendFactor / BlendOp / ColorMask values equal the D3D12 ones.
        D3D12_RENDER_TARGET_BLEND_DESC out = {};
        out.BlendEnable = target.blendEnable;
        out.LogicOpEnable = FALSE;
        out.SrcBlend = D3D12_BLEND(target.srcBlend);
        out.DestBlend = D3D12_BLEND(target.destBlend);
        out.BlendOp = D3D12_BLEND_OP(target.blendOp);
        out.SrcBlendAlpha = D3D12_BLEND(target.srcBlendAlpha);
        out.DestBlendAlpha = D3D12_BLEND(target.destBlendAlpha);
        out.BlendOpAlpha = D3D12_BLEND_OP(target.blendOpAlpha);
        out.LogicOp = D3D12_LOGIC_OP_NOOP;
        out.RenderTargetWriteMask = UINT8(target.colorWriteMask);
        return out;
    }

    D3D12_DEPTH_STENCILOP_DESC TranslateStencilOps(const nvrhi::DepthStencilState::StencilOpDesc& ops)
    {
        D3D12_DEPTH_STENCILOP_DESC out = {};
        out.StencilFailOp = D3D12_STENCIL_OP(ops.failOp);
        out.StencilDepthFailOp = D3D12_STENCIL_OP(ops.depthFailOp);
        out.StencilPassOp = D3D12_STENCIL_OP(ops.passOp);
        out.StencilFunc = D3D12_COMPARISON_FUNC(ops.stencilFunc);
        return out;
    }

    DXGI_FORMAT GetDepthStencilViewFormat(nvrhi::Format format)
    {
        switch (format)
        {
        case nvrhi::Format::D16: return DXGI_FORMAT_D16_UNORM;
        case nvrhi::Format::D24S8:
        case nvrhi::Format::X24G8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case nvrhi::Format::D32: return DXGI_FORMAT_D32_FLOAT;
        case nvrhi::Format::D32S8:
        case nvrhi::Format::X32G8_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }

    D3D12_SHADER_BYTECODE GetBytecode(nvrhi::IShader* shader)
    {
        D3D12_SHADER_BYTECODE bytecode = {};
        if (shader)
        {
            const void* data = nullptr;
            size_t size = 0;
            shader->getBytecode(&data, &size);
            bytecode.pShaderBytecode = data;
            bytecode.BytecodeLength = size;
        }
        return bytecode;
    }
}

bool IsNvMeshShaderSupported(nvrhi::IDevice* device)
{
    // Asteroids.exe: WinMain (0x14003CC90) -> 0x140001980(ID3D12Device*, &supported)
    ID3D12Device* d3dDevice = static_cast<ID3D12Device*>(
        device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device));
    if (!d3dDevice)
        return false;

    if (NvAPI_Initialize() != NVAPI_OK)
        return false;

    auto query = PFN_QueryMeshShaderSupport(nvapi_QueryInterface(c_NvApiQueryMeshShaderSupport));
    if (!query)
        return false;

    NvU8 supported = 0;
    const NvAPI_Status status = query(d3dDevice, &supported);
    return status >= 0 && supported != 0;
}

nvrhi::GraphicsPipelineHandle CreateNvMeshPipeline(nvrhi::IDevice* device, const NvMeshPipelineDesc& desc,
    const nvrhi::FramebufferInfo& framebufferInfo)
{
    // Asteroids.exe: 0x1400454A0 (meshlets), 0x140067DA0 (particles)
    ID3D12Device* d3dDevice = static_cast<ID3D12Device*>(
        device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device));
    nvrhi::d3d12::IDevice* nvrhiD3D12Device = static_cast<nvrhi::d3d12::IDevice*>(
        device->getNativeObject(nvrhi::ObjectTypes::Nvrhi_D3D12_Device));
    if (!d3dDevice || !nvrhiD3D12Device || !desc.meshShader)
        return nullptr;

    // 2018: 0x14019B5E0 (nvrhi D3D12 root signature from the pipeline's binding layouts).
    nvrhi::d3d12::RootSignatureHandle rootSignature;
    nvrhiD3D12Device->buildRootSignature(desc.bindingLayouts, false, false, nullptr, 0, &rootSignature);
    if (!rootSignature)
    {
        log::error("Failed to create a mesh shading PSO (root signature)");
        return nullptr;
    }

    const nvrhi::RenderState& state = desc.renderState;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = static_cast<ID3D12RootSignature*>(
        rootSignature->getNativeObject(nvrhi::ObjectTypes::D3D12_RootSignature));
    psoDesc.PS = GetBytecode(desc.pixelShader);     // VS stays null: the mesh / task stages come from the extensions

    psoDesc.BlendState.AlphaToCoverageEnable = state.blendState.alphaToCoverageEnable;
    psoDesc.BlendState.IndependentBlendEnable = desc.independentBlendEnable;
    for (uint32_t i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        psoDesc.BlendState.RenderTarget[i] = TranslateTargetBlend(state.blendState.targets[i]);

    psoDesc.SampleMask = ~0u;

    const nvrhi::RasterState& raster = state.rasterState;
    psoDesc.RasterizerState.FillMode = raster.fillMode == nvrhi::RasterFillMode::Wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = raster.cullMode == nvrhi::RasterCullMode::Back ? D3D12_CULL_MODE_BACK
        : raster.cullMode == nvrhi::RasterCullMode::Front ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FrontCounterClockwise = raster.frontCounterClockwise;
    psoDesc.RasterizerState.DepthBias = raster.depthBias;
    psoDesc.RasterizerState.DepthBiasClamp = raster.depthBiasClamp;
    psoDesc.RasterizerState.SlopeScaledDepthBias = raster.slopeScaledDepthBias;
    psoDesc.RasterizerState.DepthClipEnable = raster.depthClipEnable;
    psoDesc.RasterizerState.MultisampleEnable = raster.multisampleEnable;
    psoDesc.RasterizerState.AntialiasedLineEnable = raster.antialiasedLineEnable;
    psoDesc.RasterizerState.ForcedSampleCount = raster.forcedSampleCount;
    psoDesc.RasterizerState.ConservativeRaster = raster.conservativeRasterEnable
        ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    const nvrhi::DepthStencilState& depth = state.depthStencilState;
    psoDesc.DepthStencilState.DepthEnable = depth.depthTestEnable;
    psoDesc.DepthStencilState.DepthWriteMask = depth.depthWriteEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(depth.depthFunc);
    psoDesc.DepthStencilState.StencilEnable = depth.stencilEnable;
    psoDesc.DepthStencilState.StencilReadMask = depth.stencilReadMask;
    psoDesc.DepthStencilState.StencilWriteMask = depth.stencilWriteMask;
    psoDesc.DepthStencilState.FrontFace = TranslateStencilOps(depth.frontFaceStencil);
    psoDesc.DepthStencilState.BackFace = TranslateStencilOps(depth.backFaceStencil);

    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = UINT(framebufferInfo.colorFormats.size());
    for (size_t i = 0; i < framebufferInfo.colorFormats.size(); ++i)
        psoDesc.RTVFormats[i] = nvrhi::d3d12::convertFormat(framebufferInfo.colorFormats[i]);
    psoDesc.DSVFormat = GetDepthStencilViewFormat(framebufferInfo.depthFormat);
    psoDesc.SampleDesc.Count = framebufferInfo.sampleCount;
    psoDesc.SampleDesc.Quality = framebufferInfo.sampleQuality;

    // Extensions: [0] shader extension slot, [1] mesh shader, [2] task shader.
    NVAPI_D3D12_PSO_SET_SHADER_EXTENSION_SLOT_DESC slotDesc = {};
    slotDesc.baseVersion = NV_PSO_EXTENSION_DESC_VER;
    slotDesc.psoExtension = NV_PSO_SET_SHADER_EXTENSION_SLOT_AND_SPACE;
    slotDesc.version = NV_SET_SHADER_EXTENSION_SLOT_DESC_VER;
    slotDesc.uavSlot = desc.extensionUavSlot;
    slotDesc.registerSpace = desc.extensionRegisterSpace;

    const bool hasTaskShader = desc.taskShader != nullptr;
    const D3D12_SHADER_BYTECODE meshBytecode = GetBytecode(desc.meshShader);
    const D3D12_SHADER_BYTECODE taskBytecode = GetBytecode(desc.taskShader);

    nvmesh2018::NvPsoMeshShaderDesc meshDesc = {};
    meshDesc.baseVersion = nvmesh2018::c_PsoExtensionDescVer;
    meshDesc.psoExtension = NV_PSO_EXTENSION(nvmesh2018::c_PsoMeshShaderExtension);
    meshDesc.version = nvmesh2018::c_MeshShaderDescVer;
    meshDesc.pShaderBytecode = meshBytecode.pShaderBytecode;
    meshDesc.bytecodeLength = NvU32(meshBytecode.BytecodeLength);
    meshDesc.numThreads = desc.numThreads;
    meshDesc.unknown40 = hasTaskShader ? 96 : 0;
    meshDesc.unknown48 = hasTaskShader ? 1 : 0;
    meshDesc.outputTopology = nvmesh2018::c_MeshOutputTopologyTriangles;
    meshDesc.maxVertices = desc.maxVertices;
    meshDesc.maxPrimitives = desc.maxPrimitives;

    nvmesh2018::NvPsoTaskShaderDesc taskDesc = {};
    taskDesc.baseVersion = nvmesh2018::c_PsoExtensionDescVer;
    taskDesc.psoExtension = NV_PSO_EXTENSION(nvmesh2018::c_PsoTaskShaderExtension);
    taskDesc.version = nvmesh2018::c_TaskShaderDescVer;
    taskDesc.pShaderBytecode = taskBytecode.pShaderBytecode;
    taskDesc.bytecodeLength = NvU32(taskBytecode.BytecodeLength);
    taskDesc.numThreads = desc.numThreads;
    taskDesc.unknown40 = 96;

    const NVAPI_D3D12_PSO_EXTENSION_DESC* extensions[] = { &slotDesc, &meshDesc, &taskDesc };
    const NvU32 numExtensions = hasTaskShader ? 3 : 2;

    ID3D12PipelineState* pipelineState = nullptr;
    const NvAPI_Status status = NvAPI_D3D12_CreateGraphicsPipelineState(d3dDevice, &psoDesc, numExtensions, extensions, &pipelineState);
    if (status != NVAPI_OK || !pipelineState)
    {
        log::error("Failed to create a mesh shading PSO (NvAPI status %d)", int(status));
        return nullptr;
    }

    // 2018: 0x14019C990 wrapped the native PSO into an nvrhi graphics pipeline.
    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.PS = desc.pixelShader;
    pipelineDesc.renderState = desc.renderState;
    pipelineDesc.bindingLayouts = desc.bindingLayouts;

    nvrhi::GraphicsPipelineHandle pipeline;
    nvrhiD3D12Device->createHandleForNativeGraphicsPipeline(rootSignature, pipelineState, pipelineDesc, framebufferInfo, &pipeline);
    pipelineState->Release();   // the handle holds its own reference
    return pipeline;
}

bool NvDispatchMeshTasks(nvrhi::ICommandList* commandList, uint32_t numTasks)
{
    // Asteroids.exe: 0x140001AD0
    ID3D12GraphicsCommandList* d3dCommandList = static_cast<ID3D12GraphicsCommandList*>(
        commandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList));
    PFN_DispatchMeshTasks dispatch = GetDispatchMeshTasks();
    if (!d3dCommandList || !dispatch)
    {
        static bool s_ReportedMissing = false;
        if (!s_ReportedMissing)
            log::error("NVAPI DispatchMeshTasks is not available");
        s_ReportedMissing = true;
        return false;
    }

    const NvAPI_Status status = dispatch(d3dCommandList, numTasks);
    if (status != NVAPI_OK)
    {
        static bool s_Reported = false;
        if (!s_Reported)
            log::error("NVAPI DispatchMeshTasks failed (status %d)", int(status));
        s_Reported = true;
    }
    return status == NVAPI_OK;
}

#else // ASTEROIDS_WITH_NVAPI

bool IsNvMeshShaderSupported(nvrhi::IDevice*)
{
    return false;
}

nvrhi::GraphicsPipelineHandle CreateNvMeshPipeline(nvrhi::IDevice*, const NvMeshPipelineDesc&, const nvrhi::FramebufferInfo&)
{
    log::error("Failed to create a mesh shading PSO (built without NVAPI)");
    return nullptr;
}

bool NvDispatchMeshTasks(nvrhi::ICommandList*, uint32_t)
{
    return false;
}

#endif // ASTEROIDS_WITH_NVAPI

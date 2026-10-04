#include "meshlets/MeshletShaderSet.h"
#include "meshlets/NvMeshShaderApi.h"
#include "meshlets/NvMeshShaderPsoExt.h"

#include <donut/core/log.h>
#include <donut/engine/ShaderFactory.h>

#include <cstring>

using namespace donut;

namespace
{
    const char* BoolMacro(bool value)
    {
        return value ? "1" : "0";
    }

    std::string ShaderPath(const std::string& name)
    {
        // The 2018 ShaderFactory had "shaders/demo" as one of its search roots; donut's factory takes the path
        // relative to the asteroids shader output (see CONVENTIONS.md).
        return "demo/" + name;
    }

    // Task / mesh shaders: the 2018 NVAPI versions live in demo/nvapi/ (vs_6_0, see nvapi_shaders.cfg).
    std::string MeshShaderPath(MeshShaderMode mode, const std::string& name)
    {
        return mode == MeshShaderMode::Nvapi ? "demo/nvapi/" + name : "demo/" + name;
    }

    // D3D12 formats that cannot be blended (the integer formats listed in the 2018 switch).
    bool IsBlendableFormat(nvrhi::Format format)
    {
        return nvrhi::getFormatInfo(format).kind != nvrhi::FormatKind::Integer;
    }
}

MeshletShaderSet::MeshletShaderSet(nvrhi::IDevice* device, engine::ShaderFactory& shaderFactory, const MeshletShaderSetDesc& desc)
    : m_Device(device)
    , m_Mode(GetMeshShaderMode())
    , m_Desc(desc)
{
    // Asteroids.exe: 0x1400449E0
    // NVAPI mode: the 2018 shaders are vs_6_0 "vertex" shaders that the NVAPI PSO extensions turn into task / mesh
    // stages, so they are created as vertex shaders (as the 2018 shader factory did).
    const bool nvapi = (m_Mode == MeshShaderMode::Nvapi);
    const nvrhi::ShaderType taskType = nvapi ? nvrhi::ShaderType::Vertex : nvrhi::ShaderType::Amplification;
    const nvrhi::ShaderType meshType = nvapi ? nvrhi::ShaderType::Vertex : nvrhi::ShaderType::Mesh;

    std::vector<engine::ShaderMacro> taskDefines = {
        { "DEPTH_PRE_PASS", BoolMacro(desc.depthPrePass) },
        { "_MESHLETS_HI_Z", "0" }
    };
    const std::string taskFile = MeshShaderPath(m_Mode, desc.taskShader + ".hlsl");
    m_TaskShader = shaderFactory.CreateShader(taskFile.c_str(), "ts_main", &taskDefines, taskType);

    if (desc.hiZ)
    {
        taskDefines[1].definition = "1";
        m_TaskShaderHiZ = shaderFactory.CreateShader(taskFile.c_str(), "ts_main", &taskDefines, taskType);
    }

    std::vector<engine::ShaderMacro> meshDefines = {
        { "DEPTH_PRE_PASS", BoolMacro(desc.depthPrePass) }
    };
    const std::string meshFile = MeshShaderPath(m_Mode, desc.meshShader + ".hlsl");
    m_MeshShader = shaderFactory.CreateShader(meshFile.c_str(), "ms_main", &meshDefines, meshType);

    if (!desc.pixelShader.empty())
    {
        std::vector<engine::ShaderMacro> pixelDefines = {
            { "IS_SHIP", BoolMacro(desc.isShip) },
            { "_ASTEROIDS", BoolMacro(desc.asteroids) }
        };
        const std::string pixelFile = ShaderPath(desc.pixelShader);
        m_PixelShader = shaderFactory.CreateShader(pixelFile.c_str(), "main", &pixelDefines, nvrhi::ShaderType::Pixel);
    }
}

namespace
{
    // nvrhi defines operator== for BlendState only; compare the other state blocks field by field
    // (memcmp is not safe because of padding).
    bool Equal(const nvrhi::DepthStencilState::StencilOpDesc& a, const nvrhi::DepthStencilState::StencilOpDesc& b)
    {
        return a.failOp == b.failOp && a.depthFailOp == b.depthFailOp && a.passOp == b.passOp
            && a.stencilFunc == b.stencilFunc;
    }

    bool Equal(const nvrhi::DepthStencilState& a, const nvrhi::DepthStencilState& b)
    {
        return a.depthTestEnable == b.depthTestEnable
            && a.depthWriteEnable == b.depthWriteEnable
            && a.depthFunc == b.depthFunc
            && a.stencilEnable == b.stencilEnable
            && a.stencilReadMask == b.stencilReadMask
            && a.stencilWriteMask == b.stencilWriteMask
            && a.stencilRefValue == b.stencilRefValue
            && a.dynamicStencilRef == b.dynamicStencilRef
            && Equal(a.frontFaceStencil, b.frontFaceStencil)
            && Equal(a.backFaceStencil, b.backFaceStencil);
    }

    bool Equal(const nvrhi::RasterState& a, const nvrhi::RasterState& b)
    {
        return a.fillMode == b.fillMode
            && a.cullMode == b.cullMode
            && a.frontCounterClockwise == b.frontCounterClockwise
            && a.depthClipEnable == b.depthClipEnable
            && a.scissorEnable == b.scissorEnable
            && a.multisampleEnable == b.multisampleEnable
            && a.antialiasedLineEnable == b.antialiasedLineEnable
            && a.depthBias == b.depthBias
            && a.depthBiasClamp == b.depthBiasClamp
            && a.slopeScaledDepthBias == b.slopeScaledDepthBias
            && a.forcedSampleCount == b.forcedSampleCount
            && a.programmableSamplePositionsEnable == b.programmableSamplePositionsEnable
            && a.conservativeRasterEnable == b.conservativeRasterEnable
            && a.quadFillEnable == b.quadFillEnable
            && std::memcmp(a.samplePositionsX, b.samplePositionsX, sizeof(a.samplePositionsX)) == 0
            && std::memcmp(a.samplePositionsY, b.samplePositionsY, sizeof(a.samplePositionsY)) == 0;
    }

    bool Equal(const nvrhi::RenderState& a, const nvrhi::RenderState& b)
    {
        return a.blendState == b.blendState
            && Equal(a.depthStencilState, b.depthStencilState)
            && Equal(a.rasterState, b.rasterState)
            && a.singlePassStereo == b.singlePassStereo;
    }

    bool Equal(const nvrhi::BindingLayoutVector& a, const nvrhi::BindingLayoutVector& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (a[i] != b[i])
                return false;
        }
        return true;
    }
}

MeshletPipelineRef MeshletShaderSet::GetPipeline(uint32_t variant, const nvrhi::RenderState& passRenderState,
    const nvrhi::BindingLayoutVector& bindingLayouts, const nvrhi::FramebufferInfo& framebufferInfo) const
{
    // Asteroids.exe: 0x1400459C0 (there simply m_Pipelines[variant])
    if (variant >= Variant_Count)
        return {};

    for (const PipelineEntry& entry : m_Pipelines[variant])
    {
        if (entry.framebufferInfo == framebufferInfo
            && Equal(entry.bindingLayouts, bindingLayouts)
            && Equal(entry.passRenderState, passRenderState))
            return { entry.pipeline.Get(), entry.nvapiPipeline.Get() };
    }
    return {};
}

void MeshletShaderSet::ClearPipelines()
{
    for (auto& entries : m_Pipelines)
        entries.clear();
}

MeshletPipelineRef MeshletShaderSet::CreatePipeline(uint32_t variant, const nvrhi::RenderState& passRenderState,
    const nvrhi::BindingLayoutVector& bindingLayouts, const nvrhi::FramebufferInfo& framebufferInfo)
{
    // Asteroids.exe: 0x1400454A0
    // The 2018 code assembled a D3D12_GRAPHICS_PIPELINE_STATE_DESC (VS = null, PS, the converted pass render
    // state with the overrides below, SampleMask ~0, topology type TRIANGLE, SampleDesc 1/0) and created it with
    // NvAPI_D3D12_CreateGraphicsPipelineState plus the extension slot (u7) / mesh / task shader extensions, then
    // wrapped the ID3D12PipelineState into an nvrhi graphics pipeline (0x14019C990). NVAPI mode does the same
    // (CreateNvMeshPipeline); D3D12 mode creates an nvrhi meshlet pipeline with the same render state, the meshlet
    // limits (64 / 100) and the group size (32) being declared in the SM6.5 shaders.
    if (variant >= Variant_Count || !HasShaders())
        return {};

    nvrhi::IShader* taskShader = ((variant & Variant_HiZ) && m_TaskShaderHiZ) ? m_TaskShaderHiZ.Get() : m_TaskShader.Get();

    nvrhi::RenderState renderState = passRenderState;
    bool independentBlend = true;   // depth-only sets keep the pass blend desc (no colour targets anyway)

    // FillMode = ((~variant & 2) | 4) >> 1, CullMode = BACK.
    renderState.rasterState.fillMode = (variant & Variant_Wireframe) ? nvrhi::RasterFillMode::Wireframe : nvrhi::RasterFillMode::Solid;
    renderState.rasterState.cullMode = nvrhi::RasterCullMode::Back;

    if (m_Desc.depthPrePass)
    {
        // Shadow / depth rendering: constant bias for asteroids, slope-scaled bias for the basic objects.
        renderState.rasterState.depthBias = m_Desc.asteroids ? 100000 : 2;
        renderState.rasterState.slopeScaledDepthBias = m_Desc.asteroids ? 0.f : 4.f;
    }
    else
    {
        // The 2018 code zeroed the first 8 bytes of D3D12_BLEND_DESC: AlphaToCoverageEnable = FALSE and
        // IndependentBlendEnable = FALSE, so RenderTarget[0] applies to every render target.
        const bool alphaBlend = (variant & Variant_AlphaBlend) != 0;
        renderState.depthStencilState.depthWriteEnable = !alphaBlend;
        renderState.blendState.alphaToCoverageEnable = false;
        independentBlend = false;

        bool blendable = true;
        for (nvrhi::Format format : framebufferInfo.colorFormats)
            blendable = blendable && IsBlendableFormat(format);

        if (alphaBlend && blendable)
        {
            renderState.blendState.targets[0]
                .enableBlend()
                .setSrcBlend(nvrhi::BlendFactor::One)
                .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                .setBlendOp(nvrhi::BlendOp::Add);
        }

        // nvrhi always sets IndependentBlendEnable = TRUE; replicating RT0 gives the same result.
        for (uint32_t target = 1; target < nvrhi::c_MaxRenderTargets; ++target)
            renderState.blendState.targets[target] = renderState.blendState.targets[0];
    }

    PipelineEntry entry{ passRenderState, bindingLayouts, framebufferInfo, nullptr, nullptr };

    if (m_Mode == MeshShaderMode::Nvapi)
    {
        NvMeshPipelineDesc nvDesc;
        nvDesc.taskShader = taskShader;
        nvDesc.meshShader = m_MeshShader;
        nvDesc.pixelShader = m_PixelShader;
        nvDesc.renderState = renderState;
        nvDesc.independentBlendEnable = independentBlend;
        nvDesc.bindingLayouts = bindingLayouts;
        nvDesc.extensionUavSlot = 7;
        nvDesc.extensionRegisterSpace = 0;
        nvDesc.numThreads = nvmesh2018::c_MeshletGroupSize;
        nvDesc.maxVertices = nvmesh2018::c_MeshletMaxVerts;
        nvDesc.maxPrimitives = nvmesh2018::c_MeshletMaxPrims;
        entry.nvapiPipeline = CreateNvMeshPipeline(m_Device, nvDesc, framebufferInfo);   // logs on failure
    }
    else
    {
        nvrhi::MeshletPipelineDesc pipelineDesc;
        pipelineDesc.setPrimType(nvrhi::PrimitiveType::TriangleList)
            .setTaskShader(taskShader)
            .setMeshShader(m_MeshShader)
            .setPixelShader(m_PixelShader)
            .setRenderState(renderState);
        pipelineDesc.bindingLayouts = bindingLayouts;

        entry.pipeline = m_Device->createMeshletPipeline(pipelineDesc, framebufferInfo);
        if (!entry.pipeline)
            log::error("Failed to create a mesh shading PSO");
    }

    // As in 2018, a failed creation leaves no cache entry and is attempted again on the next lookup.
    if (!entry.pipeline && !entry.nvapiPipeline)
        return {};

    m_Pipelines[variant].push_back(entry);
    return { entry.pipeline.Get(), entry.nvapiPipeline.Get() };
}

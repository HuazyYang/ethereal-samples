#include "passes/TemporalAAPass2018.h"
#include "app/GpuProfiler.h"

#include <donut/engine/ShaderFactory.h>
#include <donut/engine/View.h>
#include <nvrhi/utils.h>

using namespace donut;
using namespace donut::math;
#include "../../shaders/framework/taa_cb_2018.h"

TemporalAAPass2018::TemporalAAPass2018(nvrhi::IDevice* device, std::shared_ptr<engine::ShaderFactory> shaderFactory,
    const CreateParameters& params)
    : m_Device(device)
{
    m_Textures[0] = params.resolvedColor1;
    m_Textures[1] = params.resolvedColor2;
    m_Unresolved = params.unresolvedColor;

    // 2018 ctor 0x140094C10: passes/taa_cs with SAMPLE_COUNT of the unresolved colour and USE_CATMULL_ROM_FILTER.
    const nvrhi::TextureDesc& unresolvedDesc = params.unresolvedColor->getDesc();
    std::vector<engine::ShaderMacro> macros = {
        engine::ShaderMacro("SAMPLE_COUNT", std::to_string(unresolvedDesc.sampleCount)),
        engine::ShaderMacro("USE_CATMULL_ROM_FILTER", params.useCatmullRomFilter ? "1" : "0")
    };
    m_ResolveCS = shaderFactory->CreateShader("framework/passes/taa_cs.hlsl", "main", &macros, nvrhi::ShaderType::Compute);

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.addressU = samplerDesc.addressV = samplerDesc.addressW = nvrhi::SamplerAddressMode::Border;
    samplerDesc.borderColor = nvrhi::Color(0.f);
    m_Sampler = device->createSampler(samplerDesc);

    const nvrhi::TextureDesc& resolvedDesc = params.resolvedColor1->getDesc();
    m_ResolvedColorSize = float2(float(resolvedDesc.width), float(resolvedDesc.height));

    nvrhi::BufferDesc constantBufferDesc;
    constantBufferDesc.byteSize = sizeof(taa2018::TemporalAntiAliasingConstants);
    constantBufferDesc.debugName = "TemporalAntiAliasingConstants";
    constantBufferDesc.isConstantBuffer = true;
    constantBufferDesc.isVolatile = true;
    constantBufferDesc.maxVersions = params.numConstantBufferVersions;
    m_Constants = device->createBuffer(constantBufferDesc);

    for (int i = 0; i < 2; ++i)
    {
        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_Constants),
            nvrhi::BindingSetItem::Sampler(0, m_Sampler),
            nvrhi::BindingSetItem::Texture_SRV(0, params.unresolvedColor),
            nvrhi::BindingSetItem::Texture_SRV(1, params.motionVectors),
            nvrhi::BindingSetItem::Texture_SRV(2, m_Textures[1 - i]),       // t_PrevFilteredRT
            nvrhi::BindingSetItem::Texture_UAV(0, m_Textures[i])            // u_Output
        };
        if (i == 0)
            nvrhi::utils::CreateBindingSetAndLayout(device, nvrhi::ShaderType::Compute, 0, setDesc, m_BindingLayout,
                m_BindingSets[0]);
        else
            m_BindingSets[1] = device->createBindingSet(setDesc, m_BindingLayout);
    }

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_ResolveCS;
    pipelineDesc.bindingLayouts = { m_BindingLayout };
    m_Pipeline = device->createComputePipeline(pipelineDesc);
}

void TemporalAAPass2018::Resolve(nvrhi::ICommandList* commandList, const Parameters& params, bool historyValid,
    const engine::IView& view, const engine::IView& viewPrevious)
{
    if (!historyValid)
    {
        // ResolveOrAccumulate (0x14002B2A0) without history: the HDR image seeds the output texture, no resolve.
        commandList->copyTexture(GetOutput(), nvrhi::TextureSlice(), m_Unresolved, nvrhi::TextureSlice());
        return;
    }

    demo::ProfBegin(commandList, "TemporalAA");

    const nvrhi::ViewportState viewportState = view.GetViewportState();
    const nvrhi::Rect previousExtent = viewPrevious.GetViewExtent();
    const nvrhi::Rect& scissor = viewportState.scissorRects[0];

    // TemporalResolve 0x140096BC0: the history is only read inside the previous view shrunk by a 1 pixel margin
    // (the Catmull-Rom filter reads up to 2 pixels further).
    const int margin = 1;
    taa2018::TemporalAntiAliasingConstants constants = {};
    constants.previousViewOrigin = float2(float(previousExtent.minX + margin), float(previousExtent.minY + margin));
    constants.previousViewSize = float2(float(previousExtent.maxX - previousExtent.minX - margin * 2),
        float(previousExtent.maxY - previousExtent.minY - margin * 2));
    constants.viewOrigin = float2(float(scissor.minX), float(scissor.minY));
    constants.viewSize = float2(float(scissor.maxX - scissor.minX), float(scissor.maxY - scissor.minY));
    constants.sourceTextureSizeInv = 1.f / m_ResolvedColorSize;
    constants.clampingFactor = params.enableHistoryClamping ? params.clampingFactor : -1.f;
    constants.newFrameWeight = params.newFrameWeight;
    commandList->writeBuffer(m_Constants, &constants, sizeof(constants));

    const int2 viewportSize = int2(constants.viewSize);
    const int2 gridSize = (viewportSize + 15) / 16;

    nvrhi::ComputeState state;
    state.pipeline = m_Pipeline;
    state.bindings = { m_BindingSets[m_OutputIndex] };
    commandList->setComputeState(state);
    commandList->dispatch(uint32_t(gridSize.x), uint32_t(gridSize.y), 1);

    demo::ProfEnd(commandList);
}

void TemporalAAPass2018::AdvanceFrame()
{
    m_FrameIndex = (m_FrameIndex + 1) & 7;
    m_OutputIndex = 1 - m_OutputIndex;
}

float2 TemporalAAPass2018::GetCurrentPixelOffset() const
{
    // ymmword_14025E240 / xmmword_14025E230 / xmmword_14025E260 as laid out by 0x140096310.
    static const float2 offsets[8] = {
        float2(0.0625f, -0.1875f), float2(-0.0625f, 0.1875f), float2(0.3125f, 0.0625f), float2(-0.1875f, -0.3125f),
        float2(-0.3125f, 0.3125f), float2(-0.4375f, 0.0625f), float2(0.1875f, 0.4375f), float2(0.4375f, -0.4375f)
    };
    return offsets[m_FrameIndex & 7];
}

#include "meshlets/HiZPass.h"

#include <donut/core/log.h>
#include <donut/engine/ShaderFactory.h>
#include <nvrhi/utils.h>

#include <algorithm>

using namespace donut;

namespace
{
    constexpr uint32_t c_HiZMaxLevels = 5;      // RWTexture2D<float> u_ZFar[5] : register(u0)
    constexpr uint32_t c_HiZGroupCoverage = 128; // 16 threads x 8 pixels
}

HiZPass::HiZPass(nvrhi::IDevice* device, engine::ShaderFactory& shaderFactory,
    nvrhi::ITexture* depthBuffer, nvrhi::ITexture* hiZBuffer)
    : m_Device(device)
{
    // Asteroids.exe: 0x14003FB60
    m_ComputeShader = shaderFactory.CreateShader("demo/create_hi_z_cs.hlsl", "main", nullptr, nvrhi::ShaderType::Compute);
    if (!m_ComputeShader || !depthBuffer || !hiZBuffer)
    {
        log::warning("HiZPass: missing shader or render targets, Hi-Z occlusion culling is disabled");
        return;
    }

    // Same sampler desc as MeshletRenderResources+88 (Gather ignores the filter).
    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp)
        .setAllFilters(true)
        .setMaxAnisotropy(1.f)
        .setBorderColor(nvrhi::Color(1.f));
    m_Sampler = device->createSampler(samplerDesc);

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, depthBuffer));          // t_ZBuffer
    setDesc.addItem(nvrhi::BindingSetItem::Sampler(0, m_Sampler));                // s_Sampler

    // One UAV per Hi-Z mip, u0..u(n-1), in the order of u_ZFar[].
    // unresolved: the 2018 code binds exactly mipLevels UAVs; the shader always writes 5 levels, so the
    // Hi-Z texture is expected to have (at least) 5 mips.
    const uint32_t numLevels = std::min(hiZBuffer->getDesc().mipLevels, c_HiZMaxLevels);
    for (uint32_t mip = 0; mip < numLevels; ++mip)
    {
        setDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(mip, hiZBuffer, nvrhi::Format::UNKNOWN,
            nvrhi::TextureSubresourceSet(mip, 1, 0, 1)));
    }

    // 2018: 0x14000BE30 builds the layout from the binding set desc.
    if (!nvrhi::utils::CreateBindingSetAndLayout(device, nvrhi::ShaderType::Compute, 0, setDesc, m_BindingLayout, m_BindingSet))
    {
        log::error("HiZPass: failed to create the binding set");
        return;
    }

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.setComputeShader(m_ComputeShader).addBindingLayout(m_BindingLayout);
    m_Pipeline = device->createComputePipeline(pipelineDesc);
}

void HiZPass::Dispatch(nvrhi::ICommandList* commandList, uint32_t viewWidth, uint32_t viewHeight) const
{
    // Inlined in MeshletDrawStrategy::vfunc00 (0x1400435A0).
    if (!m_Pipeline)
        return;

    nvrhi::ComputeState state;
    state.setPipeline(m_Pipeline).addBindingSet(m_BindingSet);
    commandList->setComputeState(state);
    commandList->dispatch((viewWidth + c_HiZGroupCoverage - 1) / c_HiZGroupCoverage,
        (viewHeight + c_HiZGroupCoverage - 1) / c_HiZGroupCoverage, 1);
}

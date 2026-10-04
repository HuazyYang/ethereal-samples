#pragma once

#include <nvrhi/nvrhi.h>

#include <memory>

namespace donut::engine
{
    class ShaderFactory;
}

// Asteroids.exe: Hi-Z ("far Z") pyramid builder (0x28 bytes; ctor 0x14003FB60). Created by MeshletDrawStrategy
// for the G-buffer role only and dispatched from the middle of its asteroid loop (MeshletDrawStrategy::Render),
// after the nearest sectors have been drawn: create_hi_z_cs.hlsl reduces the depth buffer drawn so far into
// the 5-level max-depth texture sampled by asteroidTS (_MESHLETS_HI_Z=1).
class HiZPass
{
public:
    // 'depthBuffer' / 'hiZBuffer' are render targets 0 and 1 of the 2018 RenderTargets object. The Hi-Z texture
    // is 1/8 of the depth resolution with (up to) 5 mips and UAV access; it is bound per mip as u_ZFar[i].
    HiZPass(nvrhi::IDevice* device, donut::engine::ShaderFactory& shaderFactory,
        nvrhi::ITexture* depthBuffer, nvrhi::ITexture* hiZBuffer);

    // One 16x16 group covers 128x128 depth pixels (16 threads x 8x8 tiles).
    void Dispatch(nvrhi::ICommandList* commandList, uint32_t viewWidth, uint32_t viewHeight) const;

    [[nodiscard]] bool IsValid() const { return m_Pipeline != nullptr; }

private:
    nvrhi::DeviceHandle m_Device;               // +0
    nvrhi::ShaderHandle m_ComputeShader;        // +8
    nvrhi::BindingLayoutHandle m_BindingLayout; // +16
    nvrhi::BindingSetHandle m_BindingSet;       // +24
    nvrhi::ComputePipelineHandle m_Pipeline;    // +32
    nvrhi::SamplerHandle m_Sampler;             // temporary in 2018 (kept alive by the binding set)
};

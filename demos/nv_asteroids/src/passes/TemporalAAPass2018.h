#pragma once

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <memory>

namespace donut::engine
{
    class IView;
    class ShaderFactory;
}

// Asteroids.exe: the 2018 framework TemporalAntiAliasingPass, resolve half (TemporalResolve 0x140096BC0,
// AdvanceFrame 0x140096290, GetCurrentPixelOffset 0x140096310); the motion vector half still comes from donut's
// pass. Differences from donut main's pass:
//  - the jitter is the fixed 8-entry table of the 2018 binary (it equals donut's MSAA table except for sample 5,
//    which is (-0.4375, +0.0625) here), indexed by the frame counter modulo 8;
//  - the resolve shader (passes/taa_cs) blends in linear HDR (no PQ encoding) into one output texture, and the history
//    is the output of the previous frame; the two resolved textures swap roles every frame (AdvanceFrame);
//  - without valid history the HDR image is copied into the output instead of resolving (ResolveOrAccumulate
//    0x14002B2A0).
class TemporalAAPass2018
{
public:
    struct CreateParameters
    {
        nvrhi::ITexture* unresolvedColor = nullptr;     // HdrColor
        nvrhi::ITexture* motionVectors = nullptr;
        nvrhi::ITexture* resolvedColor1 = nullptr;      // ResolvedColor1/2: RGBA16F UAVs, the output / history pair
        nvrhi::ITexture* resolvedColor2 = nullptr;
        bool useCatmullRomFilter = true;
        uint32_t numConstantBufferVersions = 16;
    };

    // UIData+88 (newFrameWeight 0.05, clampingFactor 1.0, enableHistoryClamping).
    struct Parameters
    {
        float newFrameWeight = 0.05f;
        float clampingFactor = 1.f;
        bool enableHistoryClamping = true;
    };

    TemporalAAPass2018(nvrhi::IDevice* device, std::shared_ptr<donut::engine::ShaderFactory> shaderFactory,
        const CreateParameters& params);

    // Resolves the current frame into GetOutput() with GetHistory() as the history. 'view' is the current planar
    // view; the previous view has the same extent in this application.
    void Resolve(nvrhi::ICommandList* commandList, const Parameters& params, bool historyValid,
        const donut::engine::IView& view, const donut::engine::IView& viewPrevious);

    // frame index = (frame index + 1) & 7; the output and history textures swap.
    void AdvanceFrame();

    // Sub-pixel jitter of the current frame.
    donut::math::float2 GetCurrentPixelOffset() const;

    nvrhi::ITexture* GetOutput() const { return m_Textures[m_OutputIndex]; }
    nvrhi::ITexture* GetHistory() const { return m_Textures[1 - m_OutputIndex]; }

private:
    nvrhi::DeviceHandle m_Device;
    nvrhi::ShaderHandle m_ResolveCS;
    nvrhi::SamplerHandle m_Sampler;
    nvrhi::BufferHandle m_Constants;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BindingSetHandle m_BindingSets[2];       // [i]: output = m_Textures[i], history = m_Textures[1 - i]
    nvrhi::ComputePipelineHandle m_Pipeline;
    nvrhi::TextureHandle m_Textures[2];
    nvrhi::TextureHandle m_Unresolved;
    donut::math::float2 m_ResolvedColorSize;
    uint32_t m_FrameIndex = 0;
    uint32_t m_OutputIndex = 0;
};

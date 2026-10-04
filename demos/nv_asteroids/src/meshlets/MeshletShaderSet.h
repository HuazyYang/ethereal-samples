#pragma once

#include "meshlets/MeshShaderMode.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <string>
#include <vector>

namespace donut::engine
{
    class ShaderFactory;
}

// Parameters of 0x1400449E0 (a4): shader file base names plus the permutation switches.
struct MeshletShaderSetDesc
{
    std::string taskShader;     // e.g. "asteroidTS" -> demo/asteroidTS.hlsl, entry ts_main
    std::string meshShader;     // e.g. "asteroidMS" -> demo/asteroidMS.hlsl, entry ms_main
    std::string pixelShader;    // e.g. "gbuffer_ps.hlsl" -> demo/gbuffer_ps.hlsl, entry main; empty = depth only
    bool depthPrePass = false;  // DEPTH_PRE_PASS (TS, MS)
    bool isShip = false;        // IS_SHIP (PS)
    bool asteroids = false;     // _ASTEROIDS (PS)
    bool hiZ = false;           // also compile the task shader with _MESHLETS_HI_Z=1
};

// The pipeline of one variant: an nvrhi meshlet pipeline (D3D12 mode) or a graphics pipeline wrapping the NVAPI
// mesh-shading PSO (NVAPI mode). Exactly one is set for a valid pipeline.
struct MeshletPipelineRef
{
    nvrhi::IMeshletPipeline* meshlet = nullptr;
    nvrhi::IGraphicsPipeline* nvapi = nullptr;

    explicit operator bool() const { return meshlet != nullptr || nvapi != nullptr; }
};

// Asteroids.exe: meshlet shader set and its pipeline cache (0xD8 bytes; ctor 0x1400449E0, pipeline lookup
// 0x1400459C0, pipeline creation 0x1400454A0). MeshletDrawStrategy owns three: basic objects (+176),
// the player ship (+184, IS_SHIP=1) and asteroids (+192, _ASTEROIDS=1).
class MeshletShaderSet
{
public:
    // Pipeline variant bits (index into the 8-entry pipeline cache).
    enum Variant : uint32_t
    {
        Variant_HiZ = 1,            // use the _MESHLETS_HI_Z=1 task shader (when it exists)
        Variant_Wireframe = 2,      // D3D12_FILL_MODE_WIREFRAME
        Variant_AlphaBlend = 4,     // premultiplied-alpha blending on RT0, no depth writes (non-depth sets only)
        Variant_Count = 8
    };

    MeshletShaderSet(nvrhi::IDevice* device, donut::engine::ShaderFactory& shaderFactory, const MeshletShaderSetDesc& desc);

    // 0x1400459C0: cached pipeline of a variant created from the same pass state, or null.
    // deviation: the 2018 cache had one slot per variant (each strategy served a single pass with a fixed render
    // state). The cache key here also contains the pass render state (blend / depth-stencil / raster, compared
    // field by field), the pass binding layouts and the framebuffer formats, because the 2018-style forward pass
    // changes MeshletPassState::renderState per material (opaque / alpha-tested / transparent) through the
    // material callback, and the ship forward strategy draws all of them.
    [[nodiscard]] MeshletPipelineRef GetPipeline(uint32_t variant, const nvrhi::RenderState& passRenderState,
        const nvrhi::BindingLayoutVector& bindingLayouts, const nvrhi::FramebufferInfo& framebufferInfo) const;

    // 0x1400454A0: creates (and caches) the pipeline of a variant. The render state, binding layouts and
    // framebuffer formats come from the calling pass (2018: copied from the pass's template graphics pipeline);
    // the shader set overrides fill mode, cull mode, depth bias / depth writes and the RT0 blend as described in
    // the .cpp. Logs "Failed to create a mesh shading PSO" and returns null on failure.
    // In NVAPI mode 'bindingLayouts' must end with the extension UAV layout (MeshletRenderResources).
    MeshletPipelineRef CreatePipeline(uint32_t variant, const nvrhi::RenderState& passRenderState,
        const nvrhi::BindingLayoutVector& bindingLayouts, const nvrhi::FramebufferInfo& framebufferInfo);

    // Drops the cached pipelines (the 2018 code recreated the whole shader set instead).
    void ClearPipelines();

    [[nodiscard]] const MeshletShaderSetDesc& GetDesc() const { return m_Desc; }
    [[nodiscard]] bool HasShaders() const { return m_TaskShader && m_MeshShader; }
    [[nodiscard]] MeshShaderMode GetMode() const { return m_Mode; }

private:
    nvrhi::DeviceHandle m_Device;                               // +0
    MeshShaderMode m_Mode;                                      // (not in 2018: NVAPI only)
    // NVAPI mode: vs_6_0 shaders of demo/nvapi/ (created as vertex shaders, as in 2018); D3D12 mode: SM6.5
    // amplification / mesh shaders of demo/.
    nvrhi::ShaderHandle m_TaskShader;                           // +8  _MESHLETS_HI_Z=0
    nvrhi::ShaderHandle m_TaskShaderHiZ;                        // +16 _MESHLETS_HI_Z=1 (only when desc.hiZ)
    nvrhi::ShaderHandle m_MeshShader;                           // +24
    nvrhi::ShaderHandle m_PixelShader;                          // +32
    // +40 was the D3D12 root signature built from the pass binding layouts (nvrhi does this now).

    struct PipelineEntry
    {
        nvrhi::RenderState passRenderState;     // as passed in (before the shader-set overrides)
        nvrhi::BindingLayoutVector bindingLayouts;
        nvrhi::FramebufferInfo framebufferInfo;
        nvrhi::MeshletPipelineHandle pipeline;          // D3D12 mode
        nvrhi::GraphicsPipelineHandle nvapiPipeline;    // NVAPI mode
    };
    std::array<std::vector<PipelineEntry>, Variant_Count> m_Pipelines; // +48 (2018: one handle per variant)
    MeshletShaderSetDesc m_Desc;                                // +112
};

#pragma once

#include <donut/app/DeviceManager.h>
#include "graphics_bindings.h"

#include "beach_scene.h"
#include "beach_assets.h"
#include "beach_shallow_water.h"
#include "main_state.h"

class BeachRenderer : public donut::app::IRenderPass {
    NVRHI_INHERIT_INTERFACE_TABLE()
 public:
    BeachRenderer(donut::app::DeviceManager* manager, MainState* state);
    bool Init();
    void Shutdown();
    bool KeyboardUpdate(int, int, int, int) override { return false; }
    bool MousePosUpdate(double, double) override { return false; }
    bool MouseButtonUpdate(int, int, int) override { return false; }
    bool ShouldAnimateUnfocused() override { return true; }
    bool ShouldRenderUnfocused() override { return true; }
    void Animate(float elapsed) override {
        if (m_state->bSimulateWater && m_state->bUpdateWindWaves)
            m_time += elapsed;
        m_shallowWater.Animate(elapsed);
    }
    void Render(nvrhi::IFramebuffer* framebuffer) override;
    void BackBufferResizing() override;
    void BackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) override;
    void RenderOpaque(nvrhi::ITexture* colorTarget, nvrhi::ITexture* linearDepthTarget,
                      nvrhi::ITexture* depthTarget,
                      nvrhi::ITimerQuery* timerQuery = nullptr);
    nvrhi::ITexture* GetBathymetryTexture() const { return m_bathymetry; }
    const BeachBathymetryMetadata& GetBathymetryMetadata() const {
        return m_bathymetryMetadata;
    }

 private:
    bool CreateShadowResources();
    bool CreatePropResources(const BeachBathymetry& bed, const BeachGroundTextures& ground);
    void DrawProps(nvrhi::GraphicsState state, bool shadow);
    void RenderShadow(const gfsdk_float3& sunDirection);
    nvrhi::FramebufferHandle GetFramebuffer(nvrhi::ITexture* colorTarget,
                                            nvrhi::ITexture* linearDepthTarget,
                                            nvrhi::ITexture* depthTarget);
    nvrhi::IDevice* m_device;
    MainState* m_state;
    nvrhi::GraphicsAPI m_backend;
    BeachShallowWater m_shallowWater;
    nvrhi::CommandListHandle m_commands;
    nvrhi::BufferHandle m_vertices, m_indices, m_frameConstants;
    nvrhi::BufferHandle m_terrainMaterialConstants;
    nvrhi::BufferHandle m_vegetationVertices, m_vegetationIndices;
    nvrhi::BufferHandle m_vegetationVsConstants, m_vegetationPsConstants;
    nvrhi::TextureHandle m_cliffAlbedo, m_sandAlbedo, m_sandNormal;
    nvrhi::TextureHandle m_grassAlbedo, m_grassNormal;
    nvrhi::TextureHandle m_groundNormal, m_groundArm, m_sandArm, m_grassArm;
    nvrhi::TextureHandle m_groundVisibility;
    nvrhi::TextureHandle m_groundLuminance;
    nvrhi::TextureHandle m_windGusts, m_bathymetry;
    nvrhi::SamplerHandle m_materialSampler;
    nvrhi::InputLayoutHandle m_inputLayout, m_vegetationInputLayout;
    nvrhi::ShaderHandle m_vs, m_ps, m_vegetationVs, m_vegetationPs;
    waveworks::graphics::GraphicsBindingLayouts m_bindingLayout, m_vegetationBindingLayout;
    waveworks::graphics::GraphicsBindingSets m_bindings, m_vegetationBindings;
    nvrhi::GraphicsPipelineHandle m_pipeline, m_vegetationPipeline;
    nvrhi::FramebufferHandle m_framebuffer;
    nvrhi::FramebufferHandle m_shadowFramebuffer;
    nvrhi::ShaderHandle m_shadowVs, m_vegetationShadowVs;
    waveworks::graphics::GraphicsBindingLayouts m_shadowLayout;
    waveworks::graphics::GraphicsBindingSets m_shadowBindings;
    nvrhi::GraphicsPipelineHandle m_shadowPipeline, m_vegetationShadowPipeline;
    BeachBathymetryMetadata m_bathymetryMetadata;
    uint32_t m_indexCount = 0, m_vegetationIndexCount = 0;
    uint32_t m_width = 1280, m_height = 800, m_samples = 1;
    bool m_recreatePipeline = false;
    float m_time = 0.f;
    struct PropBatch {
        nvrhi::BufferHandle vertices, indices, instances;
        uint32_t indexCount = 0, instanceCount = 0, kind = 0;
    };
    struct PropMaterial {
        nvrhi::TextureHandle albedo, normal, arm;
        nvrhi::BufferHandle constants;
        waveworks::graphics::GraphicsBindingSets bindings, shadowBindings;
    };
    std::vector<PropBatch> m_props;
    PropMaterial m_propMaterials[3];
    nvrhi::ShaderHandle m_propVs, m_propPs, m_propShadowVs, m_propShadowPs;
    nvrhi::InputLayoutHandle m_propInput;
    waveworks::graphics::GraphicsBindingLayouts m_propLayout, m_propShadowLayout;
    nvrhi::GraphicsPipelineHandle m_propPipeline, m_propShadowPipeline;
};

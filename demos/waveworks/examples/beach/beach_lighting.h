#pragma once

#include "main_state.h"
#include "beach_scene.h"
#include <donut/app/DeviceManager.h>

// Inverse example-world metres: art-authored clear coastal water parameters.
constexpr gfsdk_float3 BeachWaterAbsorption = {0.22f, 0.055f, 0.038f};
constexpr gfsdk_float3 BeachWaterScattering = {0.0005f, 0.002f, 0.0014f};
constexpr float BeachWaterIOR = 1.333f;

// Terrain coordinates are Y-up; ocean/atmosphere consumers explicitly swizzle.
inline gfsdk_float3 BeachSunDirection(const MainState& state) {
    return BeachSunDirectionYUp(state.fSunAngle, state.fBeachSunAzimuth);
}

// Shared by the beach's opaque and ocean renderers. Slots 0..9 remain owned
// by the surface/simulation resources; the standalone ocean never uses these.
inline void AppendBeachLightingLayout(waveworks::graphics::GraphicsBindingLayoutDesc& layout) {
    for (uint32_t slot = 10; slot <= 13; ++slot)
        layout.PS.push_back(nvrhi::BindingLayoutItem::Texture_SRV(slot));
    for (uint32_t slot : {16u, 17u})
        layout.PS.push_back(nvrhi::BindingLayoutItem::Texture_SRV(slot));
    layout.PS.push_back(nvrhi::BindingLayoutItem::Sampler(3));
    layout.PS.push_back(nvrhi::BindingLayoutItem::ConstantBuffer(2));
}

inline void AppendBeachLightingBindings(waveworks::graphics::GraphicsBindingSetDesc& bindings, MainState& state) {
    bindings.PS.push_back(nvrhi::BindingSetItem::Texture_SRV(10, state.hBeachIrradiance,
                                                             nvrhi::Format::RGBA16_FLOAT));
    bindings.PS.push_back(nvrhi::BindingSetItem::Texture_SRV(
        11, state.hBeachSpecularEnvironment, nvrhi::Format::RGBA16_FLOAT));
    bindings.PS.push_back(nvrhi::BindingSetItem::Texture_SRV(
        12, state.hBeachEnvironmentBrdf, nvrhi::Format::RG16_FLOAT));
    bindings.PS.push_back(nvrhi::BindingSetItem::Texture_SRV(13, state.hBeachShadow,
                                                             nvrhi::Format::D32));
    bindings.PS.push_back(nvrhi::BindingSetItem::Texture_SRV(16, state.hBeachAerialRadiance,
                                                             nvrhi::Format::RGBA16_FLOAT));
    bindings.PS.push_back(nvrhi::BindingSetItem::Texture_SRV(
        17, state.hBeachAerialTransmittance, nvrhi::Format::RGBA16_FLOAT));
    bindings.PS.push_back(
        nvrhi::BindingSetItem::Sampler(3, state.hBeachEnvironmentSampler));
    bindings.PS.push_back(
        nvrhi::BindingSetItem::ConstantBuffer(2, state.hBeachLightingConstants));
}

struct alignas(16) BeachLightingConstants {
    gfsdk_float4x4 worldToShadow;
    // texel size, depth bias, normal offset (m), enable shadows
    gfsdk_float4 shadow = {1.f / 2048.f, 0.000025f, 0.035f, 1.f};
    // specular maximum mip, aerial maximum distance, sea level, aerosol extinction (1/m)
    gfsdk_float4 environment = {8.f, 100000.f, 0.f, 0.f};
    gfsdk_float4 waterExtinction = {BeachWaterAbsorption.x + BeachWaterScattering.x,
                                    BeachWaterAbsorption.y + BeachWaterScattering.y,
                                    BeachWaterAbsorption.z + BeachWaterScattering.z,
                                    BeachWaterIOR};
    gfsdk_float4x4 worldToAerialClip;
    gfsdk_float4 aerialHorizon = {-1.f, 0.f, 0.f, 0.f};
};
static_assert(sizeof(BeachLightingConstants) == 192, "Beach lighting HLSL layout");

class BeachLighting {
 public:
    BeachLighting(donut::app::DeviceManager* manager, MainState* state);
    bool Init();
    bool Update();
    void Shutdown();

 private:
    bool ValidateIntegrals();
    bool ValidateAtmosphere();
    std::vector<float> ReadTexture(nvrhi::ITexture* texture, uint32_t mip,
                                   uint32_t channels);
    bool Draw(nvrhi::ITexture* target, uint32_t mip, nvrhi::IShader* shader,
              float roughness, bool sampleEnvironment,
              nvrhi::ITexture* readbackSource = nullptr,
              nvrhi::ITexture* secondTarget = nullptr);
    nvrhi::IDevice* m_device;
    nvrhi::GraphicsAPI m_backend;
    MainState* m_state;
    nvrhi::CommandListHandle m_commands;
    nvrhi::BufferHandle m_constants;
    nvrhi::TextureHandle m_multipleScattering;
    nvrhi::ShaderHandle m_vs, m_atmospherePs, m_filterPs, m_brdfPs, m_multiplePs;
    nvrhi::ShaderHandle m_readbackPs, m_aerialPs;
    waveworks::graphics::GraphicsBindingLayouts m_layout;
    waveworks::graphics::GraphicsBindingSets m_bindings;
    float m_lastSunAngle = -10000.f;
    float m_lastSunAzimuth = -10000.f;
    float m_lastViewHeight = -10000.f;
    float m_lastAerosol = -1.f;
    float m_lastFarDistance = -1.f;
    gfsdk_float4x4 m_lastViewProjection{};
    bool m_initialized = false;
};

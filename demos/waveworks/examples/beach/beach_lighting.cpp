#include <cstdint>
#include "beach_lighting.h"
#include "asset_utils.h"

#include <shaders/bytecode/BeachAtmospherePS.dxbc.h>
#include <shaders/bytecode/BeachAerialPerspectivePS.dxbc.h>
#include <shaders/bytecode/BeachAtmosphereMultiplePS.dxbc.h>
#include <shaders/bytecode/BeachEnvironmentBrdfPS.dxbc.h>
#include <shaders/bytecode/BeachEnvironmentFilterPS.dxbc.h>
#include <shaders/bytecode/BeachFullscreenVS.dxbc.h>
#include <shaders/bytecode/BeachLightingReadbackPS.dxbc.h>
#include <shaders/bytecode/BeachAtmospherePS.spirv.h>
#include <shaders/bytecode/BeachAerialPerspectivePS.spirv.h>
#include <shaders/bytecode/BeachAtmosphereMultiplePS.spirv.h>
#include <shaders/bytecode/BeachEnvironmentBrdfPS.spirv.h>
#include <shaders/bytecode/BeachEnvironmentFilterPS.spirv.h>
#include <shaders/bytecode/BeachFullscreenVS.spirv.h>
#include <shaders/bytecode/BeachLightingReadbackPS.spirv.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
constexpr uint32_t EnvironmentMipCount = 9;
constexpr float SolarIrradiance = 8.f;
struct alignas(16) EnvironmentConstants {
    gfsdk_float3 sunDirection;
    float solarIrradiance = SolarIrradiance;
    float roughness = 0.f;
    float viewHeightKm = .028f;
    float aerosolExtinction = .060f;
    float padding = 0.f;
    gfsdk_float4 viewRight = {};
    gfsdk_float4 viewUp = {};
    gfsdk_float4 viewForward = {};
};
static_assert(sizeof(EnvironmentConstants) == 80, "Environment HLSL layout");
}  // namespace

BeachLighting::BeachLighting(donut::app::DeviceManager* manager, MainState* state)
    : m_device(manager->GetDevice()),
      m_backend(manager->GetGraphicsAPI()),
      m_state(state) {}

bool BeachLighting::Init() {
    m_device->createCommandList(nvrhi::CommandListParameters(), &m_commands);
    nvrhi::BufferDesc buffer;
    buffer.byteSize = sizeof(EnvironmentConstants);
    buffer.isConstantBuffer = true;
    buffer.initialState = nvrhi::ResourceStates::ConstantBuffer;
    buffer.keepInitialState = true;
    buffer.debugName = "Beach environment integration parameters";
    m_device->createBuffer(buffer, &m_constants);
    buffer.byteSize = sizeof(BeachLightingConstants);
    buffer.debugName = "Beach directional lighting";
    m_device->createBuffer(buffer, &m_state->hBeachLightingConstants);

    auto texture = [&](uint32_t width, uint32_t height, uint32_t mips, nvrhi::Format format,
                       const char* name) {
        nvrhi::TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.mipLevels = mips;
        desc.format = format;
        desc.isRenderTarget = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName = name;
        desc.isTypeless = format == nvrhi::Format::D32;
        nvrhi::TextureHandle result;
        m_device->createTexture(desc, &result);
        return result;
    };
    m_state->hDynamicSkyDomeTexture =
        texture(512, 256, 1, nvrhi::Format::RGBA16_FLOAT, "Beach linear HDR atmosphere");
    m_multipleScattering = texture(32, 32, 1, nvrhi::Format::RGBA16_FLOAT,
                                   "Beach atmospheric multiple scattering integral");
    m_state->hBeachIrradiance =
        texture(64, 32, 1, nvrhi::Format::RGBA16_FLOAT, "Beach cosine irradiance integral");
    m_state->hBeachSpecularEnvironment =
        texture(512, 256, EnvironmentMipCount, nvrhi::Format::RGBA16_FLOAT,
                "Beach GGX environment convolution");
    m_state->hBeachEnvironmentBrdf = texture(128, 128, 1, nvrhi::Format::RG16_FLOAT,
                                             "Beach integrated Smith Schlick BRDF");
    m_state->hBeachShadow = texture(2048, 2048, 1, nvrhi::Format::D32,
                                    "Beach directional terrain and vegetation shadow");
    m_state->hBeachAerialRadiance = texture(64 * 32, 64, 1, nvrhi::Format::RGBA16_FLOAT,
                                            "Beach finite aerial inscattering");
    m_state->hBeachAerialTransmittance = texture(
        64 * 32, 64, 1, nvrhi::Format::RGBA16_FLOAT, "Beach finite aerial RGB throughput");
    nvrhi::SamplerDesc sampler;
    sampler.minFilter = sampler.magFilter = sampler.mipFilter = true;
    sampler.addressU = nvrhi::SamplerAddressMode::Wrap;
    sampler.addressV = sampler.addressW =
        nvrhi::SamplerAddressMode::Clamp;
    m_device->createSampler(sampler, &m_state->hBeachEnvironmentSampler);
    if (!m_commands || !m_constants || !m_state->hBeachLightingConstants ||
        !m_state->hDynamicSkyDomeTexture || !m_state->hBeachIrradiance ||
        !m_state->hBeachSpecularEnvironment || !m_state->hBeachEnvironmentBrdf ||
        !m_state->hBeachShadow || !m_state->hBeachEnvironmentSampler ||
        !m_multipleScattering || !m_state->hBeachAerialRadiance ||
        !m_state->hBeachAerialTransmittance)
        return false;

    const bool vk = m_backend == nvrhi::GraphicsAPI::VULKAN;
#define BEACH_SHADER(member, stage, name)                                       \
    member = waveworks::graphics::createGraphicsShader(m_device,                                             \
        nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::stage),                            \
        vk ? static_cast<const void*>(g_##name##_spirv) : g_##name##_dxbc, \
        vk ? sizeof(g_##name##_spirv) : sizeof(g_##name##_dxbc))
    BEACH_SHADER(m_vs, Vertex, BeachFullscreenVS);
    BEACH_SHADER(m_atmospherePs, Pixel, BeachAtmospherePS);
    BEACH_SHADER(m_aerialPs, Pixel, BeachAerialPerspectivePS);
    BEACH_SHADER(m_multiplePs, Pixel, BeachAtmosphereMultiplePS);
    BEACH_SHADER(m_filterPs, Pixel, BeachEnvironmentFilterPS);
    BEACH_SHADER(m_brdfPs, Pixel, BeachEnvironmentBrdfPS);
    BEACH_SHADER(m_readbackPs, Pixel, BeachLightingReadbackPS);
#undef BEACH_SHADER
    waveworks::graphics::GraphicsBindingLayoutDesc layout;
    layout.PS = {nvrhi::BindingLayoutItem::ConstantBuffer(0),
                 nvrhi::BindingLayoutItem::Texture_SRV(1),
                 nvrhi::BindingLayoutItem::Texture_SRV(0),
                 nvrhi::BindingLayoutItem::Sampler(0)};
    m_layout = waveworks::graphics::createBindingLayout(m_device, layout);
    if (!m_vs || !m_atmospherePs || !m_filterPs || !m_brdfPs || !m_multiplePs ||
        !m_readbackPs || !m_aerialPs || !m_layout)
        return false;

    // Initialize once, then preserve handles when sun edits regenerate contents.
    m_commands->open();
    for (auto& target : {m_state->hDynamicSkyDomeTexture, m_state->hBeachIrradiance,
                         m_state->hBeachSpecularEnvironment, m_state->hBeachEnvironmentBrdf,
                         m_multipleScattering, m_state->hBeachAerialRadiance,
                         m_state->hBeachAerialTransmittance}) {
        m_commands->beginTrackingTextureState(target, nvrhi::AllSubresources,
                                              nvrhi::ResourceStates::Common);
        m_commands->clearTextureFloat(
            target, nvrhi::AllSubresources,
            nvrhi::Color(target == m_state->hBeachAerialTransmittance ? 1.f : 0.f));
        m_commands->setTextureState(target, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        m_commands->commitBarriers();
    }
    m_commands->beginTrackingTextureState(m_state->hBeachShadow, nvrhi::AllSubresources,
                                          nvrhi::ResourceStates::Common);
    m_commands->clearDepthStencilTexture(m_state->hBeachShadow, nvrhi::AllSubresources,
                                         true, 1.f, false, 0);
    m_commands->setTextureState(m_state->hBeachShadow, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    m_commands->commitBarriers();
    BeachLightingConstants lighting{};
    lighting.shadow.w = 0.f;
    m_commands->writeBuffer(m_state->hBeachLightingConstants, &lighting, sizeof(lighting));
    m_commands->close();
    m_device->executeCommandList(m_commands);
    if (!Draw(m_state->hBeachEnvironmentBrdf, 0, m_brdfPs, 0.f, false))
        return false;
    if (ReadProcessEnvironmentVariable("WAVEWORKS_VALIDATE_LIGHTING") == "1" &&
        !ValidateIntegrals())
        return false;
    m_initialized = true;
    // The camera is reset only after all render passes finish initialization.
    // Integrate view-dependent atmosphere on the first Render, not with the
    // as-yet uninitialized view/projection here.
    return true;
}

bool BeachLighting::Draw(nvrhi::ITexture* target, uint32_t mip, nvrhi::IShader* shader,
                         float roughness, bool sampleEnvironment,
                         nvrhi::ITexture* readbackSource, nvrhi::ITexture* secondTarget) {
    auto framebufferDesc = nvrhi::FramebufferDesc().addColorAttachment(nvrhi::FramebufferAttachment().setTexture(target).setArraySlice(0).setMipLevel(mip));
    if (secondTarget)
        framebufferDesc.addColorAttachment(secondTarget);
    nvrhi::FramebufferHandle framebuffer;
    m_device->createFramebuffer(framebufferDesc, &framebuffer);
    waveworks::graphics::GraphicsBindingSetDesc bindings;
    bindings.PS = {nvrhi::BindingSetItem::ConstantBuffer(0, m_constants),
                   nvrhi::BindingSetItem::Texture_SRV(
                       0,
                       readbackSource      ? readbackSource
                       : sampleEnvironment ? m_state->hDynamicSkyDomeTexture.Get()
                                           : m_state->hBeachIrradiance.Get(),
                       readbackSource ? readbackSource->getDesc().format
                                      : nvrhi::Format::RGBA16_FLOAT),
                   nvrhi::BindingSetItem::Texture_SRV(1,
                                                      target == m_multipleScattering.Get()
                                                          ? m_state->hBeachIrradiance.Get()
                                                          : m_multipleScattering.Get(),
                                                      nvrhi::Format::RGBA16_FLOAT),
                   nvrhi::BindingSetItem::Sampler(0, m_state->hBeachEnvironmentSampler)};
    m_bindings = waveworks::graphics::createBindingSet(m_device, bindings, m_layout);
    nvrhi::GraphicsPipelineDesc pipeline;
    pipeline.VS = m_vs;
    pipeline.PS = shader;
    pipeline.bindingLayouts = (m_layout).layouts;
    pipeline.renderState.depthStencilState.depthTestEnable = false;
    pipeline.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    nvrhi::GraphicsPipelineHandle pso;
    if (framebuffer)
        m_device->createGraphicsPipeline1(pipeline, framebuffer->getFramebufferInfo().getInfo(), &pso);
    if (!pso || !m_bindings)
        return false;
    const auto sun = BeachSunDirection(*m_state);
    EnvironmentConstants constants;
    constants.sunDirection = {sun.x, sun.z, sun.y};
    constants.roughness = roughness;
    constants.aerosolExtinction = std::max(0.f, m_state->fBeachHaze) * 1000.f;
    if (shader == m_atmospherePs || shader == m_aerialPs)
        constants.viewHeightKm =
            std::max(.002f, (m_state->Camera.GetCameraPosition().y -
                             m_state->OceanQuadtreeParameters.mean_sea_level) *
                                .001f);
    if (shader == m_aerialPs) {
        const auto view = m_state->Camera.GetWorldToView();
        const auto projection = m_state->Camera.GetProjection();
        constants.viewRight = {view._11 / projection._11, view._31 / projection._11,
                               view._21 / projection._11, m_state->fCameraClipFar};
        constants.viewUp = {view._12 / projection._22, view._32 / projection._22,
                            view._22 / projection._22, 0.f};
        constants.viewForward = {view._13, view._33, view._23, 0.f};
    }
    if (readbackSource)
        constants.viewHeightKm =
            readbackSource->getDesc().format == nvrhi::Format::RG16_FLOAT ? 2.f : 4.f;
    m_commands->open();
    for (auto* source : {
             readbackSource ? readbackSource : sampleEnvironment
                 ? m_state->hDynamicSkyDomeTexture.Get() : m_state->hBeachIrradiance.Get(),
             target == m_multipleScattering.Get()
                 ? m_state->hBeachIrradiance.Get() : m_multipleScattering.Get()}) {
        m_commands->beginTrackingTextureState(source, nvrhi::AllSubresources,
                                              nvrhi::ResourceStates::ShaderResource);
    }
    m_commands->beginTrackingTextureState(target, nvrhi::AllSubresources,
                                          nvrhi::ResourceStates::ShaderResource);
    if (secondTarget)
        m_commands->beginTrackingTextureState(secondTarget, nvrhi::AllSubresources,
                                              nvrhi::ResourceStates::ShaderResource);
    m_commands->writeBuffer(m_constants, &constants, sizeof(constants));
    nvrhi::GraphicsState state;
    state.pipeline = pso;
    state.framebuffer = framebuffer;
    state.bindings = (m_bindings).sets();
    const uint32_t width = std::max(1u, target->getDesc().width >> mip);
    const uint32_t height = std::max(1u, target->getDesc().height >> mip);
    state.viewport.viewports = {nvrhi::Viewport(float(width), float(height))};
    state.viewport.scissorRects = {nvrhi::Rect(width, height)};
    m_commands->setGraphicsState(state);
    nvrhi::DrawArguments draw;
    draw.vertexCount = 3;
    m_commands->draw(draw);
    m_commands->setTextureState(target, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    m_commands->commitBarriers();
    if (secondTarget)
        {
            m_commands->setTextureState(secondTarget, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            m_commands->commitBarriers();
        }
    m_commands->close();
    m_device->executeCommandList(m_commands);
    return true;
}

bool BeachLighting::Update() {
    if (!m_initialized)
        return false;
    const float viewHeight =
        std::max(.002f, (m_state->Camera.GetCameraPosition().y -
                         m_state->OceanQuadtreeParameters.mean_sea_level) *
                            .001f);
    const float aerosol = std::max(0.f, m_state->fBeachHaze) * 1000.f;
    const auto viewProjection = m_state->Camera.GetWorldToClip();
    const bool mediumChanged = m_lastAerosol != aerosol;
    const bool skyChanged = mediumChanged || m_lastSunAngle != m_state->fSunAngle ||
                            m_lastSunAzimuth != m_state->fBeachSunAzimuth ||
                            m_lastViewHeight != viewHeight;
    if (mediumChanged && !Draw(m_multipleScattering, 0, m_multiplePs, 0.f, false))
        return false;
    if (skyChanged) {
        if (!Draw(m_state->hDynamicSkyDomeTexture, 0, m_atmospherePs, 0.f, false) ||
            !Draw(m_state->hBeachIrradiance, 0, m_filterPs, -1.f, true))
            return false;
        for (uint32_t mip = 0; mip < EnvironmentMipCount; ++mip)
            if (!Draw(m_state->hBeachSpecularEnvironment, mip, m_filterPs,
                      float(mip) / float(EnvironmentMipCount - 1), true))
                return false;
    }
    // Camera projection is established after render-pass initialization.
    const auto projection = m_state->Camera.GetProjection();
    if (projection._11 > 0.f && projection._22 > 0.f &&
        (skyChanged || m_lastFarDistance != m_state->fCameraClipFar ||
         std::memcmp(&viewProjection, &m_lastViewProjection, sizeof(viewProjection)))) {
        if (!Draw(m_state->hBeachAerialRadiance, 0, m_aerialPs, 0.f, false, nullptr,
                  m_state->hBeachAerialTransmittance))
            return false;
        if (ReadProcessEnvironmentVariable("WAVEWORKS_VALIDATE_LIGHTING") == "1" &&
            !ValidateAtmosphere())
            return false;
        m_lastViewProjection = viewProjection;
        m_lastFarDistance = m_state->fCameraClipFar;
    }
    // Plane-parallel solar optical depth at sea level. Same integrated density
    // columns as the spherical sky integrator (8 km Rayleigh, 0.30 km Mie,
    // 15 km ozone); only the distant sky needs spherical ray marching.
    const float elevation = std::sin(m_state->fSunAngle * 0.01745329252f);
    const float airMass = 1.f / std::max(elevation, 0.025f);
    const float visibleSun = elevation > 0.f ? SolarIrradiance : 0.f;
    m_state->vBeachSunIrradiance = {
        visibleSun *
            std::exp(-(0.005802f * 8.f + aerosol * .30f + 0.000650f * 15.f) * airMass),
        visibleSun *
            std::exp(-(0.013558f * 8.f + aerosol * .30f + 0.001881f * 15.f) * airMass),
        visibleSun *
            std::exp(-(0.033100f * 8.f + aerosol * .30f + 0.000085f * 15.f) * airMass)};
    m_lastSunAngle = m_state->fSunAngle;
    m_lastSunAzimuth = m_state->fBeachSunAzimuth;
    m_lastViewHeight = viewHeight;
    m_lastAerosol = aerosol;
    return true;
}

std::vector<float> BeachLighting::ReadTexture(nvrhi::ITexture* texture, uint32_t mip,
                                              uint32_t channels) {
    std::vector<float> result;
    std::fprintf(stderr, "GPU lighting readback: %s mip %u\n", texture->getDesc().debugName.c_str(),
                 mip);
    nvrhi::TextureDesc desc;
    desc.width = std::max(1u, texture->getDesc().width >> mip) * channels;
    desc.height = std::max(1u, texture->getDesc().height >> mip);
    desc.format = nvrhi::Format::RGBA8_UNORM;
    desc.isRenderTarget = true;
    nvrhi::TextureHandle packed;
    m_device->createTexture(desc, &packed);
    if (!packed || !m_readbackPs)
        return result;
    m_commands->open();
    m_commands->beginTrackingTextureState(packed, nvrhi::AllSubresources,
                                          nvrhi::ResourceStates::Common);
    m_commands->clearTextureFloat(packed, nvrhi::AllSubresources, nvrhi::Color(0.f));
    m_commands->setTextureState(packed, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    m_commands->commitBarriers();
    m_commands->close();
    m_device->executeCommandList(m_commands);
    if (!Draw(packed, 0, m_readbackPs, float(mip), false, texture))
        return result;
    desc.isRenderTarget = false;
    desc.isUAV = false;
    desc.isTypeless = false;
    desc.initialState = nvrhi::ResourceStates::Common;
    desc.keepInitialState = false;
    nvrhi::StagingTextureHandle staging;
    m_device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read, &staging);
    if (!staging)
        return result;
    m_commands->open();
    m_commands->beginTrackingTextureState(packed, nvrhi::AllSubresources,
                                          nvrhi::ResourceStates::ShaderResource);
    m_commands->setTextureState(packed, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
    m_commands->commitBarriers();
    m_commands->copyTexture2(staging, nvrhi::TextureSlice(), packed, nvrhi::TextureSlice());
    m_commands->setTextureState(packed, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    m_commands->commitBarriers();
    m_commands->close();
    m_device->executeCommandList(m_commands);
    m_device->waitForIdle();
    size_t pitch = 0;
    const auto* pixels = static_cast<const uint8_t*>(m_device->mapStagingTexture(
        staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, pitch));
    if (!pixels)
        return result;
    result.reserve(size_t(desc.width) * desc.height);
    for (uint32_t y = 0; y < desc.height; ++y)
        for (uint32_t x = 0; x < desc.width; ++x) {
            float value;
            std::memcpy(&value, pixels + y * pitch + x * sizeof(value), sizeof(value));
            result.push_back(value);
        }
    m_device->unmapStagingTexture(staging);
    return result;
}

bool BeachLighting::ValidateIntegrals() {
    // Read back the actual backend's shaders, not a CPU equation replica.
    auto read = [&](nvrhi::ITexture* texture, uint32_t mip, uint32_t channels) {
        return ReadTexture(texture, mip, channels);
    };
    auto fail = [](const char* message) {
        std::fprintf(stderr, "FAIL: beach GPU lighting integral: %s\n", message);
        return false;
    };
    const auto brdf = read(m_state->hBeachEnvironmentBrdf, 0, 2);
    if (brdf.size() != 128 * 128 * 2)
        return fail("BRDF readback");
    for (size_t i = 0; i < brdf.size(); i += 2) {
        const float a = brdf[i], b = brdf[i + 1];
        if (!std::isfinite(a) || !std::isfinite(b) || a < 0 || b < 0 || a + b > 1.06f ||
            0.04f * a + b > 1.01f)
            return fail("nonfinite or energy-gaining GGX/Smith/Schlick LUT");
    }
    if (brdf[127 * 2] < 0.98f || brdf[127 * 2 + 1] > 0.001f)
        return fail("smooth normal-incidence Fresnel limit");

    m_commands->open();
    m_commands->beginTrackingTextureState(m_state->hDynamicSkyDomeTexture,
                                          nvrhi::AllSubresources,
                                          nvrhi::ResourceStates::ShaderResource);
    m_commands->clearTextureFloat(m_state->hDynamicSkyDomeTexture, nvrhi::AllSubresources,
                                  nvrhi::Color(1.f));
    m_commands->setTextureState(m_state->hDynamicSkyDomeTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    m_commands->commitBarriers();
    m_commands->close();
    m_device->executeCommandList(m_commands);
    if (!Draw(m_state->hBeachIrradiance, 0, m_filterPs, -1.f, true))
        return fail("irradiance integration draw");
    const auto irradiance = read(m_state->hBeachIrradiance, 0, 4);
    if (irradiance.size() != 64 * 32 * 4)
        return fail("irradiance readback");
    for (size_t i = 0; i < irradiance.size(); ++i)
        if (i % 4 != 3 && (!std::isfinite(irradiance[i]) ||
                           std::abs(irradiance[i] - 3.14159265f) > 0.01f))
            return fail("unit environment must produce pi diffuse irradiance");

    for (uint32_t mip : {0u, 4u, 8u}) {
        if (!Draw(m_state->hBeachSpecularEnvironment, mip, m_filterPs, float(mip) / 8.f,
                  true))
            return fail("specular convolution draw");
        const auto specular = read(m_state->hBeachSpecularEnvironment, mip, 4);
        if (specular.empty())
            return fail("specular readback");
        for (float value : specular)
            if (!std::isfinite(value) || std::abs(value - 1.f) > 0.01f)
                return fail("GGX convolution must preserve a constant environment");
    }
    std::printf(
        "PASS: beach GPU lighting integrals (BRDF energy / Fresnel limit / "
        "cosine irradiance / GGX white furnace at 3 roughness levels)\n");
    std::fflush(stdout);
    return true;
}

bool BeachLighting::ValidateAtmosphere() {
    const auto sky = ReadTexture(m_state->hDynamicSkyDomeTexture, 0, 4);
    const auto radiance = ReadTexture(m_state->hBeachAerialRadiance, 0, 4);
    const auto throughput = ReadTexture(m_state->hBeachAerialTransmittance, 0, 4);
    auto fail = [](const char* message) {
        std::fprintf(stderr, "FAIL: beach GPU atmosphere: %s\n", message);
        return false;
    };
    if (sky.size() != 512 * 256 * 4 || radiance.size() != 2048 * 64 * 4 ||
        throughput.size() != radiance.size())
        return fail("sky/aerial readback dimensions");
    for (float value : sky)
        if (!std::isfinite(value) || value < 0 || value > 128.f)
            return fail("nonfinite or unbounded sky radiance");
    for (size_t i = 0; i < radiance.size(); ++i)
        if (!std::isfinite(radiance[i]) || radiance[i] < 0 || radiance[i] > 128.f ||
            !std::isfinite(throughput[i]) || throughput[i] < 0 || throughput[i] > 1.0005f) {
            std::fprintf(stderr, "Aerial sample x=%zu y=%zu channel=%zu: L=%g T=%g\n",
                         (i / 4) % 2048, i / (2048 * 4), i % 4, radiance[i], throughput[i]);
            return fail("nonfinite, negative or energy-gaining aerial samples");
        }
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 64; ++x)
            for (uint32_t c = 0; c < 3; ++c) {
                size_t base = (size_t(y) * 2048 + x) * 4 + c;
                if (radiance[base] != 0.f || throughput[base] != 1.f)
                    return fail("zero-length ray must have L=0 and T=1");
                for (uint32_t slice = 1; slice < 32; ++slice) {
                    size_t i = base + slice * 64 * 4;
                    if (throughput[i] > throughput[i - 64 * 4] + .002f ||
                        radiance[i] < radiance[i - 64 * 4] - .003f)
                        return fail(
                            "finite path integration must be monotone with distance");
                }
            }
    std::puts(
        "PASS: beach GPU atmosphere (finite sky / RGB throughput bounds / zero path / "
        "monotone volume)");
    std::fflush(stdout);
    return true;
}

void BeachLighting::Shutdown() {
    if (m_device)
        m_device->waitForIdle();
    m_bindings = nullptr;
    m_layout = nullptr;
    m_constants = nullptr;
    m_vs = nullptr;
    m_atmospherePs = nullptr;
    m_filterPs = nullptr;
    m_brdfPs = nullptr;
    m_multiplePs = nullptr;
    m_readbackPs = nullptr;
    m_aerialPs = nullptr;
    m_multipleScattering = nullptr;
    m_commands = nullptr;
    m_state->hBeachIrradiance = nullptr;
    m_state->hBeachSpecularEnvironment = nullptr;
    m_state->hBeachEnvironmentBrdf = nullptr;
    m_state->hBeachShadow = nullptr;
    m_state->hBeachAerialRadiance = nullptr;
    m_state->hBeachAerialTransmittance = nullptr;
    m_state->hBeachLightingConstants = nullptr;
    m_state->hBeachEnvironmentSampler = nullptr;
    m_state->hDynamicSkyDomeTexture = nullptr;
    m_lastSunAngle = -10000.f;
    m_lastSunAzimuth = -10000.f;
    m_lastViewHeight = -10000.f;
    m_lastAerosol = -1.f;
    m_lastFarDistance = -1.f;
    m_lastViewProjection = {};
    m_initialized = false;
}

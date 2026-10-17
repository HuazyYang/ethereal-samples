#include <cstdint>
#include "beach_renderer.h"
#include "beach_cove.h"
#include "beach_lighting.h"
#include "asset_utils.h"

#include <shaders/bytecode/BeachTerrainPS.dxbc.h>
#include <shaders/bytecode/BeachTerrainVS.dxbc.h>
#include <shaders/bytecode/BeachVegetationPS.dxbc.h>
#include <shaders/bytecode/BeachVegetationVS.dxbc.h>
#include <shaders/bytecode/BeachTerrainPS.spirv.h>
#include <shaders/bytecode/BeachTerrainVS.spirv.h>
#include <shaders/bytecode/BeachVegetationPS.spirv.h>
#include <shaders/bytecode/BeachVegetationVS.spirv.h>
#include <shaders/bytecode/BeachTerrainShadowVS.dxbc.h>
#include <shaders/bytecode/BeachVegetationShadowVS.dxbc.h>
#include <shaders/bytecode/BeachTerrainShadowVS.spirv.h>
#include <shaders/bytecode/BeachVegetationShadowVS.spirv.h>
#include <shaders/bytecode/BeachPropVS.dxbc.h>
#include <shaders/bytecode/BeachPropPS.dxbc.h>
#include <shaders/bytecode/BeachPropShadowVS.dxbc.h>
#include <shaders/bytecode/BeachPropShadowPS.dxbc.h>
#include <shaders/bytecode/BeachPropVS.spirv.h>
#include <shaders/bytecode/BeachPropPS.spirv.h>
#include <shaders/bytecode/BeachPropShadowVS.spirv.h>
#include <shaders/bytecode/BeachPropShadowPS.spirv.h>

#include <cmath>
#include <cstddef>
#include <algorithm>

namespace {
struct alignas(16) BeachFrameConstants {
    gfsdk_float4x4 viewProjection;
    gfsdk_float3 sunDirection;
    float sunIntensity;
    gfsdk_float3 eyePosition;
    float haze;
    gfsdk_float3 sunRadiance;
    float time;
    gfsdk_float3 skyIrradiance;
    float meanSeaLevel;
};
struct alignas(16) TerrainMaterialConstants {
    // Sand and ground vegetation use world metres; the rock scan retains its baked UVs.
    gfsdk_float4 uvScale = {1.f / 8.f, 1.f / 2.f, 1.f, 1.f / 0.7f};
    gfsdk_float4 roughness = {0.86f, 0.92f, 0.74f, 0.f};
    gfsdk_float4 normalStrength = {0.22f, 0.55f, 0.f, 0.12f};
    gfsdk_float4 specular = {0.3f, 0.4f, 0.4f, 0.f};
    gfsdk_float4 ao = {1.f, 1.f, 1.f, 0.f};
    // RGB: linear scan reflectance scale. W: stochastic material sampling.
    gfsdk_float4 albedoScale = {1.f, 1.f, 1.f, 0.f};
    // Art-authored pale carbonate sand reflectance, modulated by the scan.
    gfsdk_float4 sandReflectanceScale = {3.2f, 3.0f, 2.1f, 0.f};
    // Water IOR, film roughness, substrate optical depth, reserved.
    gfsdk_float4 waterCoat = {1.333f, 0.48f, 0.10f, 0.f};
    gfsdk_float4 vegetationReflectanceScale = {0.65f, 1.45f, 1.75f, 0.f};
    gfsdk_float4 groundVisibilityBounds = BeachGroundVisibilityBounds;
};
struct alignas(16) VegetationWindConstants {
    gfsdk_float4 directionSpeed;
    // Drag coefficient, bending stiffness, advection multiplier, maximum tip deflection
    // (m).
    gfsdk_float4 mechanics = {0.015f, 8.f, 1.f, 0.12f};
};
struct alignas(16) VegetationMaterialConstants {
    gfsdk_float4 surface = {0.82f, 0.5f, 1.f, 0.0003f};
    // Inverse metres: chlorophyll absorbs red/blue more strongly than green.
    gfsdk_float4 absorption = {2600.f, 700.f, 3500.f, 0.65f};
    gfsdk_float4 reflectanceScale = {0.65f, 1.45f, 1.75f, 0.f};
};
struct alignas(16) PropMaterialConstants {
    gfsdk_float4 surface;  // Foliage, normal strength, dielectric specular, alpha cutoff.
    gfsdk_float4 wind;     // World XZ direction, speed, maximum deflection.
};
static_assert(sizeof(BeachFrameConstants) == 128, "Beach frame HLSL layout");
static_assert(sizeof(TerrainMaterialConstants) == 160, "Beach material HLSL layout");
static_assert(sizeof(VegetationWindConstants) == 32, "Vegetation wind HLSL layout");
static_assert(sizeof(VegetationMaterialConstants) == 48, "Vegetation material HLSL layout");
static_assert(sizeof(BeachTerrainRenderVertex) == 36, "Terrain input stride");
static_assert(offsetof(BeachTerrainRenderVertex, packedMaterialWeights) == 32,
              "Terrain material input offset");
static_assert(sizeof(BeachVegetationVertex) == 40, "Vegetation input stride");
}  // namespace

BeachRenderer::BeachRenderer(donut::app::DeviceManager* manager, MainState* state)
    : donut::app::IRenderPass(manager),
      m_device(manager->GetDevice()),
      m_state(state),
      m_backend(manager->GetGraphicsAPI()),
      m_shallowWater(m_device, state) {}

bool BeachRenderer::Init() {
    std::vector<BeachVertex> source;
    std::vector<uint32_t> indices;
    std::vector<BeachTerrainRenderVertex> vertices;
    BeachBathymetry bathymetry;
    BeachVegetationMesh vegetation;
    BeachGroundTextures ground;
    BeachScalarTexture groundLuminance;
    groundLuminance.width = groundLuminance.height = 2;
    groundLuminance.pixels.assign(4, .5f);
    const auto media = GetMediaFolder() / "beach";
    const auto debug = ReadProcessEnvironmentVariable("WAVEWORKS_BEACH_TERRAIN_VIEW");
    const auto waterDebug = ReadProcessEnvironmentVariable("WAVEWORKS_BEACH_WATER_VIEW");
    const char* waterViews[] = {"lit",        "optical-depth",  "transmission",
                                "reflection", "refraction-hit", "pde-depth",
                                "normal",     "pde-foam",       "pde-wetness"};
    for (int i = 0; i < 9; ++i)
        if (waterDebug == waterViews[i])
            m_state->iBeachWaterDebug = i;
    if (debug == "albedo")
        m_state->iBeachTerrainDebug = 1;
    if (debug == "normal")
        m_state->iBeachTerrainDebug = 2;
    if (debug == "coverage")
        m_state->iBeachTerrainDebug = 3;
    if (debug == "sky-visibility")
        m_state->iBeachTerrainDebug = 4;
    if (ReadProcessEnvironmentVariable("WAVEWORKS_BEACH_NO_FOAM") == "1")
        m_state->bBeachShoreFoam = false;
    if (ReadProcessEnvironmentVariable("WAVEWORKS_BEACH_NO_SHADOWS") == "1")
        m_state->bBeachShadows = false;
    BeachTerrainClassification classification;
    classification.terrainDatumSeaLevel = m_state->OceanQuadtreeParameters.mean_sea_level;
    if (m_state->bBeachCoveTerrain) {
        if (!ground.Load((media / "scans").generic_string()) ||
            !groundLuminance.Load(
                (media / "scans/coastal_ground_luminance.bin").generic_string()))
            return false;
        BeachCoveSettings settings;
        settings.ground = &ground;
        settings.meanSeaLevel = classification.terrainDatumSeaLevel;
        if (!BuildBeachCove(settings, source, indices))
            return false;
    } else if (!LoadBeachTerrainMesh((media / "hanalei_cove.bin").generic_string().c_str(),
                                     source, indices)) {
        return false;
    }
    if (!BuildBeachTerrainRenderVertices(source, classification, vertices) ||
        !BuildBeachBathymetry(source, bathymetry))
        return false;
    if (m_state->bBeachCoveTerrain)
        ClassifyBeachCoveGround(classification.terrainDatumSeaLevel, ground, vertices);
    std::vector<float> groundVisibility(1, 1.f);
    const uint32_t visibilitySize =
        m_state->bBeachCoveTerrain && ReadProcessEnvironmentVariable(
                                          "WAVEWORKS_BEACH_NO_TERRAIN_OCCLUSION") != "1"
            ? 512
            : 1;
    if (visibilitySize > 1 &&
        !BuildBeachGroundVisibility(bathymetry, BeachGroundVisibilityBounds, visibilitySize,
                                    visibilitySize, groundVisibility))
        return false;
    std::printf("Beach terrain: %s, %zu vertices, %zu indices\n",
                m_state->bBeachCoveTerrain ? "cove" : "survey", source.size(),
                indices.size());
    BeachVegetationSettings vegetationSettings;
    if (m_state->bBeachCoveTerrain) {
        // Imported species replace the previous single-triangle grass cards.
        vegetationSettings.maximumBladeCount = 0;
    }
    if (!BuildBeachVegetationMesh(vertices, bathymetry.metadata, vegetationSettings,
                                  vegetation) ||
        !m_shallowWater.Init(bathymetry))
        return false;
    m_bathymetryMetadata = bathymetry.metadata;
    m_indexCount = uint32_t(indices.size());
    m_vegetationIndexCount = uint32_t(vegetation.indices.size());
    m_device->createCommandList(nvrhi::CommandListParameters(), &m_commands);
    if (!m_commands)
        return false;
    m_commands->open();
    auto upload = [&](const void* data, size_t bytes, bool isIndex, const char* name) {
        nvrhi::BufferDesc desc;
        desc.byteSize = bytes;
        desc.isVertexBuffer = !isIndex;
        desc.isIndexBuffer = isIndex;
        desc.debugName = name;
        nvrhi::BufferHandle buffer;
        m_device->createBuffer(desc, &buffer);
        if (buffer) {
            m_commands->beginTrackingBufferState(buffer, nvrhi::ResourceStates::Common);
            m_commands->writeBuffer(buffer, data, bytes);
            m_commands->setPermanentBufferState(buffer, isIndex
                                                   ? nvrhi::ResourceStates::IndexBuffer
                                                   : nvrhi::ResourceStates::VertexBuffer);
            m_commands->commitBarriers();
        }
        return buffer;
    };
    m_vertices = upload(vertices.data(), vertices.size() * sizeof(vertices[0]), false,
                        "Beach terrain vertices");
    m_indices = upload(indices.data(), indices.size() * sizeof(uint32_t), true,
                       "Beach terrain indices");
    if (m_vegetationIndexCount) {
        m_vegetationVertices =
            upload(vegetation.vertices.data(),
                   vegetation.vertices.size() * sizeof(BeachVegetationVertex), false,
                   "Coastal grass blades");
        m_vegetationIndices =
            upload(vegetation.indices.data(), vegetation.indices.size() * sizeof(uint32_t),
                   true, "Coastal grass indices");
    }
    nvrhi::TextureDesc depth;
    depth.width = bathymetry.metadata.width;
    depth.height = bathymetry.metadata.height;
    depth.format = nvrhi::Format::R32_FLOAT;
    depth.debugName = "Authored cove bathymetry";
    m_device->createTexture(depth, &m_bathymetry);
    if (!m_vertices || !m_indices || !m_bathymetry ||
        (m_vegetationIndexCount && (!m_vegetationVertices || !m_vegetationIndices))) {
        m_commands->close();
        return false;
    }
    m_commands->beginTrackingTextureState(m_bathymetry, nvrhi::AllSubresources,
                                          nvrhi::ResourceStates::Common);
    m_commands->writeTexture(m_bathymetry, 0, 0, bathymetry.elevations.data(),
                             depth.width * sizeof(float));
    m_commands->setPermanentTextureState(m_bathymetry, nvrhi::ResourceStates::ShaderResource);
    m_commands->commitBarriers();
    nvrhi::TextureDesc visibilityDesc;
    visibilityDesc.width = visibilityDesc.height = visibilitySize;
    visibilityDesc.format = nvrhi::Format::R32_FLOAT;
    visibilityDesc.debugName = "Beach height-field cosine sky visibility";
    m_device->createTexture(visibilityDesc, &m_groundVisibility);
    if (!m_groundVisibility) {
        m_commands->close();
        return false;
    }
    m_commands->beginTrackingTextureState(m_groundVisibility, nvrhi::AllSubresources,
                                          nvrhi::ResourceStates::Common);
    m_commands->writeTexture(m_groundVisibility, 0, 0, groundVisibility.data(),
                             visibilitySize * sizeof(float));
    m_commands->setPermanentTextureState(m_groundVisibility, nvrhi::ResourceStates::ShaderResource);
    m_commands->commitBarriers();
    visibilityDesc.width = groundLuminance.width;
    visibilityDesc.height = groundLuminance.height;
    visibilityDesc.debugName = "Beach source-derived inverse luminance histograms";
    m_device->createTexture(visibilityDesc, &m_groundLuminance);
    if (!m_groundLuminance) {
        m_commands->close();
        return false;
    }
    m_commands->beginTrackingTextureState(m_groundLuminance, nvrhi::AllSubresources,
                                          nvrhi::ResourceStates::Common);
    m_commands->writeTexture(m_groundLuminance, 0, 0, groundLuminance.pixels.data(),
                             groundLuminance.width * sizeof(float));
    m_commands->setPermanentTextureState(m_groundLuminance, nvrhi::ResourceStates::ShaderResource);
    m_commands->commitBarriers();

    auto constantBuffer = [&](size_t bytes, const char* name) {
        nvrhi::BufferDesc desc;
        desc.byteSize = bytes;
        desc.isConstantBuffer = true;
        desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
        desc.keepInitialState = true;
        desc.debugName = name;
        nvrhi::BufferHandle result;
        m_device->createBuffer(desc, &result);
        return result;
    };
    m_frameConstants = constantBuffer(sizeof(BeachFrameConstants), "Beach frame");
    m_terrainMaterialConstants =
        constantBuffer(sizeof(TerrainMaterialConstants), "Terrain material");
    m_vegetationVsConstants =
        constantBuffer(sizeof(VegetationWindConstants), "Vegetation wind");
    m_vegetationPsConstants =
        constantBuffer(sizeof(VegetationMaterialConstants), "Vegetation material");
    auto texture = [&](const char* filename, bool color) {
        return CreateTextureFromFile(
            m_device, m_commands, (media / filename).generic_string().c_str(),
            color ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM, true);
    };
    m_sandAlbedo = texture(m_state->bBeachCoveTerrain ? "scans/sand_albedo.dds"
                                                      : "aerial_beach_02_diff_1k.dds",
                           true);
    m_sandNormal = texture(m_state->bBeachCoveTerrain ? "scans/sand_normal.dds"
                                                      : "aerial_beach_02_nor_dx_1k.dds",
                           false);
    if (m_state->bBeachCoveTerrain) {
        m_cliffAlbedo = texture("scans/coastal_ground_albedo.dds", true);
        m_grassAlbedo = texture("scans/vegetation_ground_albedo.dds", true);
        m_groundNormal = texture("scans/coastal_ground_normal.dds", false);
        m_grassNormal = texture("scans/vegetation_ground_normal.dds", false);
        m_grassArm = texture("scans/vegetation_ground_arm.dds", false);
        m_groundArm = texture("scans/coastal_ground_arm.dds", false);
        m_sandArm = texture("scans/sand_arm.dds", false);
    } else {
        m_cliffAlbedo = texture("coastal_cliff_01_diff_2k.dds", true);
        m_grassAlbedo = texture("sparse_grass_diff_1k.dds", true);
        m_grassNormal = texture("sparse_grass_nor_dx_1k.dds", false);
        // Bound but unused by the original surveyed terrain's material branch.
        m_groundNormal = m_grassNormal;
        m_groundArm = m_sandArm = m_grassArm = m_grassNormal;
    }
    m_windGusts = CreateTextureFromFile(
        m_device, m_commands,
        (GetMediaFolder() / "wind_gusts.dds").generic_string().c_str());
    m_commands->close();
    m_device->executeCommandList(m_commands);
    if (!m_frameConstants || !m_terrainMaterialConstants || !m_vegetationVsConstants ||
        !m_vegetationPsConstants || !m_cliffAlbedo || !m_sandAlbedo || !m_sandNormal ||
        !m_grassAlbedo || !m_grassNormal || !m_groundNormal || !m_groundArm || !m_sandArm ||
        !m_grassArm || !m_windGusts)
        return false;

    const bool vk = m_backend == nvrhi::GraphicsAPI::VULKAN;
#define BEACH_SHADER(member, stage, name)                                       \
    member = waveworks::graphics::createGraphicsShader(m_device,                                             \
        nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::stage),                            \
        vk ? static_cast<const void*>(g_##name##_spirv) : g_##name##_dxbc, \
        vk ? sizeof(g_##name##_spirv) : sizeof(g_##name##_dxbc))
    BEACH_SHADER(m_vs, Vertex, BeachTerrainVS);
    BEACH_SHADER(m_ps, Pixel, BeachTerrainPS);
    BEACH_SHADER(m_vegetationVs, Vertex, BeachVegetationVS);
    BEACH_SHADER(m_vegetationPs, Pixel, BeachVegetationPS);
#undef BEACH_SHADER
    if (!m_vs || !m_ps || !m_vegetationVs || !m_vegetationPs)
        return false;
    const nvrhi::VertexAttributeDesc terrainAttributes[] = {
        {"POSITION", nvrhi::Format::RGB32_FLOAT, 1, 0, 0, 36, false},
        {"NORMAL", nvrhi::Format::RGB32_FLOAT, 1, 0, 12, 36, false},
        {"TEXCOORD", nvrhi::Format::RG32_FLOAT, 1, 0, 24, 36, false},
        {"COLOR", nvrhi::Format::RGBA8_UNORM, 1, 0, 32, 36, false}};
    const nvrhi::VertexAttributeDesc grassAttributes[] = {
        {"POSITION", nvrhi::Format::RGB32_FLOAT, 1, 0, 0, 40, false},
        {"FACING", nvrhi::Format::RG32_FLOAT, 1, 0, 12, 40, false},
        {"BLADE", nvrhi::Format::RG32_FLOAT, 1, 0, 20, 40, false},
        {"MATERIALUV", nvrhi::Format::RG32_FLOAT, 1, 0, 28, 40, false},
        {"WINDWEIGHT", nvrhi::Format::R32_FLOAT, 1, 0, 36, 40, false}};
    m_device->createInputLayout(terrainAttributes, 4, m_vs, &m_inputLayout);
    m_device->createInputLayout(grassAttributes, 5, m_vegetationVs, &m_vegetationInputLayout);
    nvrhi::SamplerDesc sampler;
    sampler.minFilter = sampler.magFilter = sampler.mipFilter = true;
    sampler.maxAnisotropy = 8.f;
    sampler.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    m_device->createSampler(sampler, &m_materialSampler);
    waveworks::graphics::GraphicsBindingLayoutDesc layout;
    layout.VS = {nvrhi::BindingLayoutItem::ConstantBuffer(0)};
    layout.PS = {nvrhi::BindingLayoutItem::Texture_SRV(0),
                 nvrhi::BindingLayoutItem::Texture_SRV(1),
                 nvrhi::BindingLayoutItem::Texture_SRV(2),
                 nvrhi::BindingLayoutItem::Texture_SRV(3),
                 nvrhi::BindingLayoutItem::Texture_SRV(4),
                 nvrhi::BindingLayoutItem::Texture_SRV(5),
                 nvrhi::BindingLayoutItem::Texture_SRV(6),
                 nvrhi::BindingLayoutItem::Texture_SRV(7),
                 nvrhi::BindingLayoutItem::Texture_SRV(8),
                 nvrhi::BindingLayoutItem::Texture_SRV(9),
                 nvrhi::BindingLayoutItem::Texture_SRV(18),
                 nvrhi::BindingLayoutItem::Sampler(0),
                 nvrhi::BindingLayoutItem::ConstantBuffer(0),
                 nvrhi::BindingLayoutItem::ConstantBuffer(1)};
    AppendBeachLightingLayout(layout);
    AppendBeachShallowLayout(layout, false);
    m_bindingLayout = waveworks::graphics::createBindingLayout(m_device, layout);
    waveworks::graphics::GraphicsBindingSetDesc bindings;
    bindings.VS = {nvrhi::BindingSetItem::ConstantBuffer(0, m_frameConstants)};
    bindings.PS = {
        nvrhi::BindingSetItem::Texture_SRV(0, m_cliffAlbedo, nvrhi::Format::SRGBA8_UNORM),
        nvrhi::BindingSetItem::Texture_SRV(1, m_sandAlbedo, nvrhi::Format::SRGBA8_UNORM),
        nvrhi::BindingSetItem::Texture_SRV(2, m_sandNormal, nvrhi::Format::RGBA8_UNORM),
        nvrhi::BindingSetItem::Texture_SRV(3, m_grassAlbedo, nvrhi::Format::SRGBA8_UNORM),
        nvrhi::BindingSetItem::Texture_SRV(4, m_grassNormal, nvrhi::Format::RGBA8_UNORM),
        nvrhi::BindingSetItem::Texture_SRV(5, m_groundNormal, nvrhi::Format::RGBA8_UNORM),
        nvrhi::BindingSetItem::Texture_SRV(6, m_groundArm, nvrhi::Format::RGBA8_UNORM),
        nvrhi::BindingSetItem::Texture_SRV(7, m_sandArm, nvrhi::Format::RGBA8_UNORM),
        nvrhi::BindingSetItem::Texture_SRV(8, m_grassArm, nvrhi::Format::RGBA8_UNORM),
        nvrhi::BindingSetItem::Texture_SRV(9, m_groundVisibility, nvrhi::Format::R32_FLOAT),
        nvrhi::BindingSetItem::Texture_SRV(18, m_groundLuminance, nvrhi::Format::R32_FLOAT),
        nvrhi::BindingSetItem::Sampler(0, m_materialSampler),
        nvrhi::BindingSetItem::ConstantBuffer(0, m_frameConstants),
        nvrhi::BindingSetItem::ConstantBuffer(1, m_terrainMaterialConstants)};
    AppendBeachLightingBindings(bindings, *m_state);
    AppendBeachShallowBindings(bindings, *m_state, false);
    if (!m_bindingLayout)
        return false;
    m_bindings = waveworks::graphics::createBindingSet(m_device, bindings, m_bindingLayout);
    layout.VS = {nvrhi::BindingLayoutItem::ConstantBuffer(0),
                 nvrhi::BindingLayoutItem::ConstantBuffer(1),
                 nvrhi::BindingLayoutItem::Texture_SRV(0),
                 nvrhi::BindingLayoutItem::Sampler(0)};
    layout.PS = {nvrhi::BindingLayoutItem::ConstantBuffer(0),
                 nvrhi::BindingLayoutItem::ConstantBuffer(1),
                 nvrhi::BindingLayoutItem::Texture_SRV(0),
                 nvrhi::BindingLayoutItem::Sampler(0)};
    AppendBeachLightingLayout(layout);
    m_vegetationBindingLayout = waveworks::graphics::createBindingLayout(m_device, layout);
    bindings.VS = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_frameConstants),
        nvrhi::BindingSetItem::ConstantBuffer(1, m_vegetationVsConstants),
        nvrhi::BindingSetItem::Texture_SRV(0, m_windGusts, nvrhi::Format::RGBA8_UNORM),
        nvrhi::BindingSetItem::Sampler(0, m_materialSampler)};
    bindings.PS = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_frameConstants),
        nvrhi::BindingSetItem::ConstantBuffer(1, m_vegetationPsConstants),
        nvrhi::BindingSetItem::Texture_SRV(0, m_grassAlbedo, nvrhi::Format::SRGBA8_UNORM),
        nvrhi::BindingSetItem::Sampler(0, m_materialSampler)};
    AppendBeachLightingBindings(bindings, *m_state);
    if (!m_vegetationBindingLayout)
        return false;
    m_vegetationBindings = waveworks::graphics::createBindingSet(m_device, bindings, m_vegetationBindingLayout);
    return m_bindings && m_vegetationBindings && m_inputLayout && m_vegetationInputLayout &&
           m_materialSampler && CreateShadowResources() &&
           (!m_state->bBeachCoveTerrain || CreatePropResources(bathymetry, ground));
}

bool BeachRenderer::CreateShadowResources() {
    const bool vk = m_backend == nvrhi::GraphicsAPI::VULKAN;
    m_shadowVs = waveworks::graphics::createGraphicsShader(m_device, 
        nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex),
        vk ? static_cast<const void*>(g_BeachTerrainShadowVS_spirv)
           : g_BeachTerrainShadowVS_dxbc,
        vk ? sizeof(g_BeachTerrainShadowVS_spirv)
           : sizeof(g_BeachTerrainShadowVS_dxbc));
    m_vegetationShadowVs = waveworks::graphics::createGraphicsShader(m_device, 
        nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex),
        vk ? static_cast<const void*>(g_BeachVegetationShadowVS_spirv)
           : g_BeachVegetationShadowVS_dxbc,
        vk ? sizeof(g_BeachVegetationShadowVS_spirv)
           : sizeof(g_BeachVegetationShadowVS_dxbc));
    m_device->createFramebuffer(nvrhi::FramebufferDesc().setDepthAttachment(m_state->hBeachShadow), &m_shadowFramebuffer);
    waveworks::graphics::GraphicsBindingLayoutDesc layout;
    layout.VS = {nvrhi::BindingLayoutItem::ConstantBuffer(0),
                 nvrhi::BindingLayoutItem::ConstantBuffer(1),
                 nvrhi::BindingLayoutItem::ConstantBuffer(2),
                 nvrhi::BindingLayoutItem::Texture_SRV(0),
                 nvrhi::BindingLayoutItem::Sampler(0)};
    m_shadowLayout = waveworks::graphics::createBindingLayout(m_device, layout);
    if (!m_shadowVs || !m_vegetationShadowVs || !m_shadowFramebuffer || !m_shadowLayout)
        return false;
    waveworks::graphics::GraphicsBindingSetDesc bindings;
    bindings.VS = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_frameConstants),
        nvrhi::BindingSetItem::ConstantBuffer(1, m_vegetationVsConstants),
        nvrhi::BindingSetItem::ConstantBuffer(2, m_state->hBeachLightingConstants),
        nvrhi::BindingSetItem::Texture_SRV(0, m_windGusts, nvrhi::Format::RGBA8_UNORM),
        nvrhi::BindingSetItem::Sampler(0, m_materialSampler)};
    m_shadowBindings = waveworks::graphics::createBindingSet(m_device, bindings, m_shadowLayout);
    nvrhi::GraphicsPipelineDesc pipeline;
    pipeline.VS = m_shadowVs;
    pipeline.inputLayout = m_inputLayout;
    pipeline.bindingLayouts = (m_shadowLayout).layouts;
    pipeline.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    pipeline.renderState.rasterState.slopeScaledDepthBias = 1.5f;
    pipeline.renderState.rasterState.depthBias = 64;
    pipeline.renderState.depthStencilState.depthTestEnable = true;
    pipeline.renderState.depthStencilState.depthWriteEnable =
        true;
    pipeline.renderState.depthStencilState.depthFunc =
        nvrhi::ComparisonFunc::LessOrEqual;
    m_device->createGraphicsPipeline1(pipeline, m_shadowFramebuffer->getFramebufferInfo().getInfo(), &m_shadowPipeline);
    pipeline.VS = m_vegetationShadowVs;
    pipeline.inputLayout = m_vegetationInputLayout;
    m_device->createGraphicsPipeline1(pipeline, m_shadowFramebuffer->getFramebufferInfo().getInfo(), &m_vegetationShadowPipeline);
    return m_shadowBindings && m_shadowPipeline && m_vegetationShadowPipeline;
}

bool BeachRenderer::CreatePropResources(const BeachBathymetry& bed,
                                        const BeachGroundTextures& ground) {
    const auto folder = GetMediaFolder() / "beach" / "scans";
    const bool vk = m_backend == nvrhi::GraphicsAPI::VULKAN;
#define PROP_SHADER(member, stage, name)                                        \
    member = waveworks::graphics::createGraphicsShader(m_device,                                             \
        nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::stage),                            \
        vk ? static_cast<const void*>(g_##name##_spirv) : g_##name##_dxbc, \
        vk ? sizeof(g_##name##_spirv) : sizeof(g_##name##_dxbc))
    PROP_SHADER(m_propVs, Vertex, BeachPropVS);
    PROP_SHADER(m_propPs, Pixel, BeachPropPS);
    PROP_SHADER(m_propShadowVs, Vertex, BeachPropShadowVS);
    PROP_SHADER(m_propShadowPs, Pixel, BeachPropShadowPS);
#undef PROP_SHADER
    const nvrhi::VertexAttributeDesc attributes[] = {
        {"POSITION", nvrhi::Format::RGB32_FLOAT, 1, 0, 0, 32, false},
        {"NORMAL", nvrhi::Format::RGB32_FLOAT, 1, 0, 12, 32, false},
        {"TEXCOORD", nvrhi::Format::RG32_FLOAT, 1, 0, 24, 32, false},
        {"INSTANCE", nvrhi::Format::RGBA32_FLOAT, 2, 1, 0, 32, true}};
    if (!m_propVs || !m_propPs || !m_propShadowVs || !m_propShadowPs)
        return false;
    m_device->createInputLayout(attributes, 4, m_propVs, &m_propInput);
    waveworks::graphics::GraphicsBindingLayoutDesc layout;
    layout.VS = {nvrhi::BindingLayoutItem::ConstantBuffer(0),
                 nvrhi::BindingLayoutItem::ConstantBuffer(1),
                 nvrhi::BindingLayoutItem::ConstantBuffer(2),
                 nvrhi::BindingLayoutItem::Texture_SRV(0),
                 nvrhi::BindingLayoutItem::Sampler(0)};
    layout.PS = {nvrhi::BindingLayoutItem::ConstantBuffer(1),
                 nvrhi::BindingLayoutItem::Texture_SRV(0),
                 nvrhi::BindingLayoutItem::Sampler(0)};
    m_propShadowLayout = waveworks::graphics::createBindingLayout(m_device, layout);
    layout.PS.push_back(nvrhi::BindingLayoutItem::ConstantBuffer(0));
    layout.PS.push_back(nvrhi::BindingLayoutItem::Texture_SRV(1));
    layout.PS.push_back(nvrhi::BindingLayoutItem::Texture_SRV(2));
    AppendBeachLightingLayout(layout);
    m_propLayout = waveworks::graphics::createBindingLayout(m_device, layout);
    if (!m_propInput || !m_propLayout || !m_propShadowLayout)
        return false;
    const char* slugs[] = {"rock_moss_set_01", "grass_bermuda_01", "shrub_sorrel_01"};
    for (uint32_t kind = 0; kind < 3; ++kind) {
        std::vector<BeachPropMesh> meshes;
        BeachPropInstances instances;
        if (!LoadBeachPropMeshes(
                (folder / (std::string(slugs[kind]) + ".mesh")).generic_string(), meshes) ||
            !BuildBeachPropInstances(meshes, kind, bed, ground, instances))
            return false;
        auto& material = m_propMaterials[kind];
        nvrhi::BufferDesc cb;
        cb.byteSize = sizeof(PropMaterialConstants);
        cb.isConstantBuffer = true;
        cb.initialState = nvrhi::ResourceStates::ConstantBuffer;
        cb.keepInitialState = true;
        m_device->createBuffer(cb, &material.constants);
        m_commands->open();
        auto texture = [&](const char* suffix, bool color) {
            return CreateTextureFromFile(
                m_device, m_commands,
                (folder / (std::string(slugs[kind]) + suffix)).generic_string().c_str(),
                color ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM, true);
        };
        material.albedo = texture("_albedo.dds", true);
        material.normal = texture("_normal.dds", false);
        material.arm = texture("_arm.dds", false);
        auto upload = [&](const void* data, size_t bytes, bool index) {
            nvrhi::BufferDesc desc;
            desc.byteSize = bytes;
            desc.isVertexBuffer = !index;
            desc.isIndexBuffer = index;
            nvrhi::BufferHandle buffer;
            m_device->createBuffer(desc, &buffer);
            if (buffer) {
                m_commands->beginTrackingBufferState(buffer, nvrhi::ResourceStates::Common);
                m_commands->writeBuffer(buffer, data, bytes);
                m_commands->setPermanentBufferState(buffer, index ? nvrhi::ResourceStates::IndexBuffer
                          : nvrhi::ResourceStates::VertexBuffer);
                m_commands->commitBarriers();
            }
            return buffer;
        };
        size_t totalInstances = 0;
        bool validBuffers = true;
        for (size_t i = 0; i < meshes.size(); ++i) {
            if (instances[i].empty())
                continue;
            const auto& mesh = meshes[i];
            PropBatch batch;
            batch.kind = kind;
            batch.indexCount = uint32_t(mesh.indices.size());
            batch.instanceCount = uint32_t(instances[i].size());
            totalInstances += batch.instanceCount;
            batch.vertices = upload(mesh.vertices.data(),
                                    mesh.vertices.size() * sizeof(BeachVertex), false);
            batch.indices = upload(mesh.indices.data(), mesh.indices.size() * 4, true);
            batch.instances =
                upload(instances[i].data(), instances[i].size() * sizeof(BeachPropInstance),
                       false);
            validBuffers =
                validBuffers && batch.vertices && batch.indices && batch.instances;
            m_props.push_back(batch);
        }
        m_commands->close();
        m_device->executeCommandList(m_commands);
        if (!validBuffers || !material.albedo || !material.normal || !material.arm ||
            !material.constants)
            return false;
        waveworks::graphics::GraphicsBindingSetDesc bindings;
        bindings.VS = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_frameConstants),
            nvrhi::BindingSetItem::ConstantBuffer(1, material.constants),
            nvrhi::BindingSetItem::ConstantBuffer(2, m_state->hBeachLightingConstants),
            nvrhi::BindingSetItem::Texture_SRV(0, m_windGusts),
            nvrhi::BindingSetItem::Sampler(0, m_materialSampler)};
        bindings.PS = {nvrhi::BindingSetItem::ConstantBuffer(1, material.constants),
                       nvrhi::BindingSetItem::Texture_SRV(0, material.albedo,
                                                          nvrhi::Format::SRGBA8_UNORM),
                       nvrhi::BindingSetItem::Sampler(0, m_materialSampler)};
        material.shadowBindings = waveworks::graphics::createBindingSet(m_device, bindings, m_propShadowLayout);
        bindings.PS.push_back(nvrhi::BindingSetItem::ConstantBuffer(0, m_frameConstants));
        bindings.PS.push_back(nvrhi::BindingSetItem::Texture_SRV(1, material.normal));
        bindings.PS.push_back(nvrhi::BindingSetItem::Texture_SRV(2, material.arm));
        AppendBeachLightingBindings(bindings, *m_state);
        material.bindings = waveworks::graphics::createBindingSet(m_device, bindings, m_propLayout);
        if (!material.bindings || !material.shadowBindings)
            return false;
        std::printf("Beach scanned props: %s, %zu variants, %zu instances\n", slugs[kind],
                    meshes.size(), totalInstances);
    }
    nvrhi::GraphicsPipelineDesc pipeline;
    pipeline.VS = m_propShadowVs;
    pipeline.PS = m_propShadowPs;
    pipeline.inputLayout = m_propInput;
    pipeline.bindingLayouts = (m_propShadowLayout).layouts;
    pipeline.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    pipeline.renderState.depthStencilState.depthFunc =
        nvrhi::ComparisonFunc::LessOrEqual;
    m_device->createGraphicsPipeline1(pipeline, m_shadowFramebuffer->getFramebufferInfo().getInfo(), &m_propShadowPipeline);
    return bool(m_propShadowPipeline);
}

void BeachRenderer::DrawProps(nvrhi::GraphicsState state, bool shadow) {
    if (m_props.empty())
        return;
    state.pipeline = shadow ? m_propShadowPipeline : m_propPipeline;
    if (!state.pipeline)
        return;
    for (const auto& batch : m_props) {
        if (batch.kind && !m_state->bRenderBeachVegetation)
            continue;
        const auto& material = m_propMaterials[batch.kind];
        state.bindings = (shadow ? material.shadowBindings : material.bindings).sets();
        state.vertexBuffers = {{batch.vertices, 0, 0}, {batch.instances, 1, 0}};
        state.indexBuffer = {batch.indices, nvrhi::Format::R32_UINT, 0};
        m_commands->setGraphicsState(state);
        nvrhi::DrawArguments draw;
        draw.vertexCount = batch.indexCount;
        draw.instanceCount = batch.instanceCount;
        m_commands->drawIndexed(draw);
    }
}

void BeachRenderer::RenderShadow(const gfsdk_float3& sunDirection) {
    BeachLightingConstants lighting{};
    const auto right = BeachSunShadowRight(m_state->fBeachSunAzimuth);
    const gfsdk_float3 forward = -1.f * sunDirection;
    const gfsdk_float3 up = cross(forward, right);
    gfsdk_float3 center = m_state->Camera.GetCameraPosition();
    center.x += 60.f;
    center.y = 0.f;
    constexpr float halfExtent = 192.f, depthExtent = 600.f;
    constexpr float texel = 2.f * halfExtent / 2048.f;
    const float cx = std::floor(dot(right, center) / texel) * texel;
    const float cy = std::floor(dot(up, center) / texel) * texel;
    auto& matrix = lighting.worldToShadow;
    matrix._11 = right.x / halfExtent;
    matrix._21 = right.y / halfExtent;
    matrix._31 = right.z / halfExtent;
    matrix._41 = -cx / halfExtent;
    matrix._12 = up.x / halfExtent;
    matrix._22 = up.y / halfExtent;
    matrix._32 = up.z / halfExtent;
    matrix._42 = -cy / halfExtent;
    matrix._13 = forward.x / depthExtent;
    matrix._23 = forward.y / depthExtent;
    matrix._33 = forward.z / depthExtent;
    matrix._43 = 0.5f - dot(forward, center) / depthExtent;
    matrix._44 = 1.f;
    lighting.worldToAerialClip = m_state->Camera.GetWorldToClip();
    const auto view = m_state->Camera.GetWorldToView();
    const auto projection = m_state->Camera.GetProjection();
    const float viewUpY = view._22 / projection._22;
    if (std::abs(viewUpY) > 1e-5f)
        lighting.aerialHorizon = {.5f + .5f * view._23 / viewUpY,
                                  view._21 / projection._11 / viewUpY, 0, 0};
    lighting.environment.y = m_state->fCameraClipFar;
    lighting.environment.z = m_state->OceanQuadtreeParameters.mean_sea_level;
    lighting.environment.w = m_state->fBeachHaze;
    lighting.shadow.w = m_state->bBeachShadows && m_state->bRenderBeachTerrain ? 1.f : 0.f;
    m_commands->writeBuffer(m_state->hBeachLightingConstants, &lighting, sizeof(lighting));
    if (lighting.shadow.w == 0.f)
        return;
    m_commands->beginTrackingTextureState(m_state->hBeachShadow, nvrhi::AllSubresources,
                                          nvrhi::ResourceStates::ShaderResource);
    m_commands->clearDepthStencilTexture(m_state->hBeachShadow, nvrhi::AllSubresources,
                                         true, 1.f, false, 0);
    nvrhi::GraphicsState state;
    state.pipeline = m_shadowPipeline;
    state.framebuffer = m_shadowFramebuffer;
    state.bindings = (m_shadowBindings).sets();
    state.vertexBuffers = {{m_vertices, 0, 0}};
    state.indexBuffer = {m_indices, nvrhi::Format::R32_UINT, 0};
    state.viewport.viewports = {nvrhi::Viewport(2048.f, 2048.f)};
    state.viewport.scissorRects = {nvrhi::Rect(2048, 2048)};
    m_commands->setGraphicsState(state);
    nvrhi::DrawArguments draw;
    draw.vertexCount = m_indexCount;
    m_commands->drawIndexed(draw);
    if (m_vegetationIndexCount && m_state->bRenderBeachVegetation) {
        state.pipeline = m_vegetationShadowPipeline;
        state.vertexBuffers = {{m_vegetationVertices, 0, 0}};
        state.indexBuffer = {m_vegetationIndices, nvrhi::Format::R32_UINT, 0};
        m_commands->setGraphicsState(state);
        draw.vertexCount = m_vegetationIndexCount;
        m_commands->drawIndexed(draw);
    }
    DrawProps(state, true);
    m_commands->setTextureState(m_state->hBeachShadow, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    m_commands->commitBarriers();
}

void BeachRenderer::Shutdown() {
    m_device->waitForIdle();
    m_props.clear();
    for (auto& material : m_propMaterials)
        material = {};
    m_propPipeline = m_propShadowPipeline = nullptr;
    m_propVs = m_propPs = m_propShadowVs = m_propShadowPs = nullptr;
    m_propLayout = m_propShadowLayout = nullptr;
    m_propInput = nullptr;
    m_groundNormal = m_groundArm = m_sandArm = m_grassArm = nullptr;
    m_groundVisibility = nullptr;
    m_groundLuminance = nullptr;
    m_shallowWater.Shutdown();
    m_shadowFramebuffer = nullptr;
    m_shadowPipeline = nullptr;
    m_vegetationShadowPipeline = nullptr;
    m_shadowBindings = nullptr;
    m_shadowLayout = nullptr;
    m_shadowVs = nullptr;
    m_vegetationShadowVs = nullptr;
    m_framebuffer = nullptr;
    m_pipeline = nullptr;
    m_vegetationPipeline = nullptr;
    m_bindings = nullptr;
    m_vegetationBindings = nullptr;
    m_bindingLayout = nullptr;
    m_vegetationBindingLayout = nullptr;
    m_materialSampler = nullptr;
    m_cliffAlbedo = nullptr;
    m_sandAlbedo = nullptr;
    m_sandNormal = nullptr;
    m_grassAlbedo = nullptr;
    m_grassNormal = nullptr;
    m_windGusts = nullptr;
    m_bathymetry = nullptr;
    m_frameConstants = nullptr;
    m_terrainMaterialConstants = nullptr;
    m_vegetationVsConstants = nullptr;
    m_vegetationPsConstants = nullptr;
    m_indices = nullptr;
    m_vertices = nullptr;
    m_vegetationVertices = nullptr;
    m_vegetationIndices = nullptr;
    m_inputLayout = nullptr;
    m_vegetationInputLayout = nullptr;
    m_vs = nullptr;
    m_ps = nullptr;
    m_vegetationVs = nullptr;
    m_vegetationPs = nullptr;
    m_commands = nullptr;
}

void BeachRenderer::BackBufferResizing() {
    m_framebuffer = nullptr;
}

void BeachRenderer::BackBufferResized(uint32_t width, uint32_t height, uint32_t samples) {
    m_width = width;
    m_height = height;
    m_samples = samples;
    m_recreatePipeline = true;
}

nvrhi::FramebufferHandle BeachRenderer::GetFramebuffer(nvrhi::ITexture* colorTarget,
                                                       nvrhi::ITexture* linearDepthTarget,
                                                       nvrhi::ITexture* depthTarget) {
    if (!m_framebuffer)
        m_device->createFramebuffer(nvrhi::FramebufferDesc()
                                            .addColorAttachment(colorTarget)
                                            .addColorAttachment(linearDepthTarget)
                                            .setDepthAttachment(depthTarget), &m_framebuffer);
    if (!m_framebuffer)
        return nullptr;
    if (!m_pipeline || !m_vegetationPipeline || m_recreatePipeline) {
        nvrhi::GraphicsPipelineDesc pipeline;
        pipeline.VS = m_vs;
        pipeline.PS = m_ps;
        pipeline.inputLayout = m_inputLayout;
        pipeline.bindingLayouts = (m_bindingLayout).layouts;
        pipeline.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Back;
        pipeline.renderState.depthStencilState.depthTestEnable = true;
        pipeline.renderState.depthStencilState.depthWriteEnable =
            true;
        pipeline.renderState.depthStencilState.depthFunc =
            nvrhi::ComparisonFunc::LessOrEqual;
        m_device->createGraphicsPipeline1(pipeline, m_framebuffer->getFramebufferInfo().getInfo(), &m_pipeline);
        pipeline.VS = m_vegetationVs;
        pipeline.PS = m_vegetationPs;
        pipeline.inputLayout = m_vegetationInputLayout;
        pipeline.bindingLayouts = (m_vegetationBindingLayout).layouts;
        pipeline.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        m_device->createGraphicsPipeline1(pipeline, m_framebuffer->getFramebufferInfo().getInfo(), &m_vegetationPipeline);
        if (!m_props.empty()) {
            pipeline.VS = m_propVs;
            pipeline.PS = m_propPs;
            pipeline.inputLayout = m_propInput;
            pipeline.bindingLayouts = (m_propLayout).layouts;
            m_device->createGraphicsPipeline1(pipeline, m_framebuffer->getFramebufferInfo().getInfo(), &m_propPipeline);
        }
        m_recreatePipeline = false;
    }
    return m_framebuffer;
}

void BeachRenderer::Render(nvrhi::IFramebuffer*) {}

void BeachRenderer::RenderOpaque(nvrhi::ITexture* colorTarget,
                                 nvrhi::ITexture* linearDepthTarget,
                                 nvrhi::ITexture* depthTarget,
                                 nvrhi::ITimerQuery* timerQuery) {
    m_shallowWater.Upload();
    auto framebuffer = GetFramebuffer(colorTarget, linearDepthTarget, depthTarget);
    if (!framebuffer || !m_pipeline || !m_vegetationPipeline ||
        (!m_props.empty() && !m_propPipeline))
        return;
    m_commands->open();
    m_commands->beginTrackingTextureState(colorTarget, nvrhi::AllSubresources,
                                          nvrhi::ResourceStates::RenderTarget);
    m_commands->beginTrackingTextureState(linearDepthTarget, nvrhi::AllSubresources,
                                          nvrhi::ResourceStates::RenderTarget);
    m_commands->beginTrackingTextureState(depthTarget, nvrhi::AllSubresources,
                                          nvrhi::ResourceStates::DepthWrite);
    BeachFrameConstants frame{};
    frame.viewProjection = m_state->Camera.GetWorldToClip();
    frame.eyePosition = m_state->Camera.GetCameraPosition();
    frame.sunDirection = BeachSunDirection(*m_state);
    frame.sunIntensity = m_state->fSunIntensity;
    frame.sunRadiance = m_state->vBeachSunIrradiance;
    frame.haze = m_state->fBeachHaze;
    frame.time = m_time;
    frame.meanSeaLevel = m_state->OceanQuadtreeParameters.mean_sea_level;
    // The legacy frame-layout irradiance field remains reserved. Directional
    // sky irradiance now comes from the shared HDR lighting resources.
    TerrainMaterialConstants material;
    material.specular.x = m_state->fBeachSandSpecular;
    material.normalStrength.x = m_state->fBeachSandNormalStrength;
    material.waterCoat.y = m_state->fBeachWetSandRoughness;
    material.waterCoat.w = float(m_state->iBeachTerrainDebug);
    VegetationMaterialConstants leaf;
    if (m_state->bBeachCoveTerrain) {
        // Material input calibration only: pale dry sand, sparse sun-dried
        // coastal ground cover, and smaller physical scan footprints.
        material.uvScale = {1.f / BeachSandScanSize, 1.f / BeachVegetationScanSize,
                            1.f / (0.12f * BeachGroundScanSize), 1.f / 0.45f};
        material.sandReflectanceScale = {3.35f, 3.25f, 2.65f, 0.f};
        material.vegetationReflectanceScale = {0.15f, 0.22f, 0.08f, 0.f};
        material.albedoScale.z = 1.10f;
        material.normalStrength.x *= 0.32f;
        material.normalStrength.y = 0.65f;
        material.normalStrength.z = 1.0f;
        material.normalStrength.w = 0.07f;
        material.albedoScale.w = 1.f;  // Enable stochastic input sampling.
        leaf.reflectanceScale = material.vegetationReflectanceScale;
    }
    VegetationWindConstants wind;
    const auto& parameters = m_state->OceanWindSimulationParameters;
    const float windSpeed =
        m_state->OceanWindSimulationSettings.use_Beaufort_scale
            ? 0.836f * std::pow(std::max(parameters.base_wind_speed, 0.f), 1.5f)
            : parameters.base_wind_speed;
    wind.directionSpeed = {parameters.base_wind_direction.x,
                           parameters.base_wind_direction.y, windSpeed, 0.025f};
    m_commands->writeBuffer(m_frameConstants, &frame, sizeof(frame));
    m_commands->writeBuffer(m_terrainMaterialConstants, &material, sizeof(material));
    m_commands->writeBuffer(m_vegetationVsConstants, &wind, sizeof(wind));
    m_commands->writeBuffer(m_vegetationPsConstants, &leaf, sizeof(leaf));
    for (uint32_t kind = 0; kind < 3; ++kind) {
        if (!m_propMaterials[kind].constants)
            continue;
        PropMaterialConstants props{
            {kind ? 1.f : 0.f, 1.f, .5f, kind ? .45f : -1.f},
            {parameters.base_wind_direction.x, parameters.base_wind_direction.y, windSpeed,
             kind ? .04f : 0.f}};
        m_commands->writeBuffer(m_propMaterials[kind].constants, &props, sizeof(props));
    }
    RenderShadow(frame.sunDirection);
    if (!m_state->bRenderBeachTerrain) {
        m_commands->setTextureState(linearDepthTarget, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        m_commands->commitBarriers();
        m_commands->setTextureState(depthTarget, nvrhi::AllSubresources, nvrhi::ResourceStates::DepthRead);
        m_commands->commitBarriers();
        m_commands->close();
        m_device->executeCommandList(m_commands);
        return;
    }
    nvrhi::GraphicsState state;
    state.pipeline = m_pipeline;
    state.framebuffer = framebuffer;
    state.bindings = (m_bindings).sets();
    state.vertexBuffers = {{m_vertices, 0, 0}};
    state.indexBuffer = {m_indices, nvrhi::Format::R32_UINT, 0};
    state.viewport.viewports = {nvrhi::Viewport(float(m_width), float(m_height))};
    state.viewport.scissorRects = {nvrhi::Rect(m_width, m_height)};
    if (timerQuery)
        m_commands->beginTimerQuery(timerQuery);
    m_commands->setGraphicsState(state);
    nvrhi::DrawArguments draw;
    draw.vertexCount = m_indexCount;
    m_commands->drawIndexed(draw);
    if (m_vegetationIndexCount && m_state->bRenderBeachVegetation) {
        state.pipeline = m_vegetationPipeline;
        state.bindings = (m_vegetationBindings).sets();
        state.vertexBuffers = {{m_vegetationVertices, 0, 0}};
        state.indexBuffer = {m_vegetationIndices, nvrhi::Format::R32_UINT, 0};
        m_commands->setGraphicsState(state);
        draw.vertexCount = m_vegetationIndexCount;
        m_commands->drawIndexed(draw);
    }
    DrawProps(state, false);
    if (timerQuery)
        m_commands->endTimerQuery(timerQuery);
    m_commands->setTextureState(colorTarget, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    m_commands->commitBarriers();
    m_commands->setTextureState(linearDepthTarget, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    m_commands->commitBarriers();
    m_commands->setTextureState(depthTarget, nvrhi::AllSubresources, nvrhi::ResourceStates::DepthRead);
    m_commands->commitBarriers();
    m_commands->close();
    m_device->executeCommandList(m_commands);
}

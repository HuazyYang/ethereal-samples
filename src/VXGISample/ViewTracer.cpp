#include "ViewTracer.h"
#include "VoxelRenderer.h"
#include "VoxelTexture.h"
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/BindingCache.h>
#include <donut/core/log.h>

#include "shaders/ConeTracing/ConeTracingConstants.hlsli"

#include <donut/core/vfs/VFS.h>
#include <donut/engine/TextureCache.h>
#include <nvrhi/utils.h>

namespace vxgi {

static const float g_RandomValuesSmall[16] = {0.059,      0.52899998, 0.176, 0.64700001, 0.76499999, 0.294,
                                              0.88200003, 0.412,      0.235, 0.70599997, 0.118,      0.588,
                                              0.94099998, 0.47099999, 0.824, 0.35299999};

ViewTracer::ViewTracer(VoxelRenderer *parent, bool ambientOcclusion)
    : m_Parent(parent) {
    m_Device = m_Parent->GetDevice();
    m_ShaderFactory = m_Parent->GetShaderFactory();

    m_AmbientOcclusionMode = ambientOcclusion;

    memset(&m_PreviousDiffuseTracingParams, 0, sizeof(m_PreviousDiffuseTracingParams));
}

ViewTracer::~ViewTracer() {}

Status ViewTracer::AllocateResources(nvrhi::ICommandList *commandList) {
    Status rc;

    {
        const nvrhi::Format dsFormatCandidates[] = {nvrhi::Format::D24S8, nvrhi::Format::D32,
                                                    nvrhi::Format::D16, nvrhi::Format::D32S8};
        const nvrhi::FormatSupport dsFormatFeatures = nvrhi::FormatSupport::Texture |
                                                      nvrhi::FormatSupport::DepthStencil |
                                                      nvrhi::FormatSupport::ShaderLoad;
        m_AvailableDSFormat =
            nvrhi::utils::ChooseFormat(m_Device, dsFormatFeatures, dsFormatCandidates, dim(dsFormatCandidates));
    }

    auto params = m_Parent->GetVoxelizationParameters();
    // m_AmbientOcclusionMode = params->emittanceFormat == EmittanceFormat::NONE;
    m_DownsampledTargetCount = m_AmbientOcclusionMode ? 1 : 3;

    m_pVisualizeSamplesVS = m_ShaderFactory->CreateShader("app/ConeTracingDebug/VisualizeSamplesVS.hlsl",
                                                          "main", nullptr, nvrhi::ShaderType::Vertex);
    m_pVisualizeSamplesGS = m_ShaderFactory->CreateShader("app/ConeTracingDebug/VisualizeSamplesGS.hlsl",
                                                          "main", nullptr, nvrhi::ShaderType::Geometry);
    m_pVisualizeSamplesPS = m_ShaderFactory->CreateShader("app/ConeTracingDebug/VisualizeSamplesPS.hlsl",
                                                          "main", nullptr, nvrhi::ShaderType::Pixel);
    m_pVisualizeTexelsGS = m_ShaderFactory->CreateShader("app/ConeTracingDebug/VisualizeTexelsGS.hlsl", "main",
                                                         nullptr, nvrhi::ShaderType::Geometry);
    m_pVisualizeTexelsPS = m_ShaderFactory->CreateShader("app/ConeTracingDebug/VisualizeTexelsPS.hlsl", "main",
                                                         nullptr, nvrhi::ShaderType::Pixel);
    m_pVisualizeConesGS = m_ShaderFactory->CreateShader("app/ConeTracingDebug/VisualizeConesGS.hlsl", "main",
                                                        nullptr, nvrhi::ShaderType::Geometry);
    m_pVisualizeConesPS = m_ShaderFactory->CreateShader("app/ConeTracingDebug/VisualizeConesPS.hlsl", "main",
                                                        nullptr, nvrhi::ShaderType::Pixel);

    nvrhi::BufferDesc bufDesc;
    bufDesc.isConstantBuffer = true;
    bufDesc.isVolatile = true;
    bufDesc.maxVersions = m_Parent->GetNumFramesInFlight() * 4;
    bufDesc.byteSize = sizeof(BuiltinTracingConstants);
    bufDesc.debugName = "ViewTracer:BuiltinTracing";
    m_pTracingCB = m_Device->createBuffer(bufDesc);

    bufDesc.byteSize = sizeof(DownsampleConstants);
    bufDesc.debugName = "ViewTracer:Downsample";
    m_DownsampleCB = m_Device->createBuffer(bufDesc);

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Border);
    samplerDesc.borderColor = nvrhi::Color{0.f};
    samplerDesc.minFilter = false;
    m_pSamplerLinearBorder = m_Device->createSampler(samplerDesc);

    nvrhi::SamplerDesc envMapSampler = {};
    m_EnvironmentMapSampler = m_Device->createSampler(envMapSampler);

    nvrhi::SamplerDesc pointSampler;
    pointSampler.setAllFilters(false);
    pointSampler.setAllAddressModes(nvrhi::SamplerAddressMode::Border);
    pointSampler.borderColor = nvrhi::Color{0.f};
    m_PointSampler = m_Device->createSampler(pointSampler);

    bufDesc = {};
    bufDesc.structStride = sizeof(SampleData);
    bufDesc.byteSize = 4096 * sizeof(SampleData);
    bufDesc.canHaveUAVs = true;
    m_SamplePositions = m_Device->createBuffer(bufDesc);

    bufDesc.structStride = 32;
    bufDesc.byteSize = 129 * 32;
    m_ConeDirections = m_Device->createBuffer(bufDesc);

    uint drawIndirectInitialData[] = {0, 1, 0, 0, 0, 1, 0, 0};
    bufDesc = {};
    bufDesc.byteSize = sizeof(drawIndirectInitialData);
    bufDesc.isDrawIndirectArgs = true;
    bufDesc.initialState = nvrhi::ResourceStates::CopyDest;
    m_DrawIndirectArgs = m_Device->createBuffer(bufDesc);
    commandList->beginTrackingBufferState(m_DrawIndirectArgs, nvrhi::ResourceStates::CopyDest);
    commandList->writeBuffer(m_DrawIndirectArgs, drawIndirectInitialData, sizeof(drawIndirectInitialData));

    bufDesc = {};
    bufDesc.byteSize = 64;
    bufDesc.canHaveUAVs = 1;
    bufDesc.isDrawIndirectArgs = 1;
    m_AppendCounters = m_Device->createBuffer(bufDesc);

    bufDesc = {};
    bufDesc.isConstantBuffer = true;
    bufDesc.isVolatile = true;
    bufDesc.maxVersions = m_Parent->GetNumFramesInFlight();
    bufDesc.byteSize = sizeof(VisualizeSamplesConstants);
    bufDesc.debugName = "ViewTracer:VisualizeSamples";
    m_VisualizeSamplesCB = m_Device->createBuffer(bufDesc);

    nvrhi::TextureDesc randTextureDesc;
    randTextureDesc.format = nvrhi::Format::R32_FLOAT;
    randTextureDesc.dimension = nvrhi::TextureDimension::Texture2D;
    randTextureDesc.width = 16;
    randTextureDesc.height = 16;
    randTextureDesc.debugName = "randTextureLarge";
    float *randomValues = new float[16 * 16];
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            int z = zcurve(bitrev(y, 4), bitrev(y ^ x, 4), 4);
            randomValues[y * 16 + x] = (float)z / 256.f;
        }
    }
    m_RandomsTextureLarge = m_Device->createTexture(randTextureDesc);
    commandList->beginTrackingTextureState(m_RandomsTextureLarge, nvrhi::AllSubresources,
                                           nvrhi::ResourceStates::CopyDest);
    commandList->writeTexture(m_RandomsTextureLarge, 0, 0, randomValues, 16 * sizeof(float));
    commandList->setPermanentTextureState(m_RandomsTextureLarge, nvrhi::ResourceStates::ShaderResource);
    delete[] randomValues;

    randTextureDesc.width = 4;
    randTextureDesc.height = 4;
    randTextureDesc.debugName = "randTextureSmall";
    m_RandomsTextureSmall = m_Device->createTexture(randTextureDesc);
    commandList->beginTrackingTextureState(m_RandomsTextureSmall, nvrhi::AllSubresources,
                                           nvrhi::ResourceStates::CopyDest);
    commandList->writeTexture(m_RandomsTextureSmall, 0, 0, g_RandomValuesSmall, 4 * sizeof(float));
    commandList->setPermanentTextureState(m_RandomsTextureSmall, nvrhi::ResourceStates::ShaderResource);

    uint nullImageValue = 0xff000000;

    nvrhi::TextureDesc nullTextureDesc;
    nullTextureDesc.format = nvrhi::Format::RGBA8_UNORM;
    nullTextureDesc.dimension = nvrhi::TextureDimension::Texture2D;
    nullTextureDesc.width = 1;
    nullTextureDesc.height = 1;
    nullTextureDesc.debugName = "nullTexture";
    m_NullTexture = m_Device->createTexture(nullTextureDesc);

    nullTextureDesc.isTypeless = true;
    nullTextureDesc.isRenderTarget = true;
    nullTextureDesc.format = m_AvailableDSFormat;
    nullTextureDesc.isRenderTarget = true;
    nullTextureDesc.isTypeless = true;
    nullTextureDesc.debugName = "nullDepthStencil";
    m_NullDepthStencilTexture = m_Device->createTexture(nullTextureDesc);

    nvrhi::TextureDesc nullCubemapDesc;
    nullCubemapDesc.format = nvrhi::Format::RGBA8_UNORM;
    nullCubemapDesc.dimension = nvrhi::TextureDimension::TextureCube;
    nullCubemapDesc.width = 1;
    nullCubemapDesc.height = 1;
    nullCubemapDesc.arraySize = 6;
    nullCubemapDesc.debugName = "nullCubemap";
    m_NullCubemap = m_Device->createTexture(nullCubemapDesc);

    commandList->beginTrackingTextureState(m_NullTexture, nvrhi::AllSubresources,
                                           nvrhi::ResourceStates::CopyDest);
    commandList->beginTrackingTextureState(m_NullDepthStencilTexture, nvrhi::AllSubresources,
                                           nvrhi::ResourceStates::DepthWrite);
    commandList->beginTrackingTextureState(m_NullCubemap, nvrhi::AllSubresources,
                                           nvrhi::ResourceStates::CopyDest);

    commandList->writeTexture(m_NullTexture, 0, 0, &nullImageValue, 0);
    commandList->clearDepthStencilTexture(m_NullDepthStencilTexture, nvrhi::AllSubresources, true, 0.f, true,
                                          0);
    for (uint i = 0; i < 6; ++i)
        commandList->writeTexture(m_NullCubemap, i, 0, &nullImageValue, 0);

    commandList->setPermanentTextureState(m_NullTexture, nvrhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(m_NullDepthStencilTexture, nvrhi::ResourceStates::ShaderResource);
    commandList->setPermanentTextureState(m_NullCubemap, nvrhi::ResourceStates::ShaderResource);

    {
        nvrhi::BindingLayoutDesc bindingLayoutDesc;
        bindingLayoutDesc.visibility = nvrhi::ShaderType::Compute;
        bindingLayoutDesc.registerSpaceIsDescriptorSet = true;
        bindingLayoutDesc.bindings = {
            nvrhi::BindingLayoutItem::PushConstants(0, 3 * sizeof(float)),  // FullScreenQuadCB
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(
                VXGI_CT_BUILTIN_CBV_SLOT),                                          // cBuiltinTracingParameters
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_DEPTH_BUFFER_SRV_SLOT),   // g_DepthBuffer
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_NORMAL_BUFFER_SRV_SLOT),  // g_TargetNormal
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_FLAT_NORMAL_BUFFER_SRV_SLOT),  // g_TargetFlatNormal
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_STENCIL_BUFFER_SRV_SLOT),      // g_TargetStencil
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_PREV_DEPTH_BUFFER_SRV_SLOT),   // g_PrevDepthBuffer
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_PREV_NORMAL_BUFFER_SRV_SLOT),  // g_PrevTargetNormal
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_0),           //
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_1),           //
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_2),           //
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_3),           //
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_4),           //
            nvrhi::BindingLayoutItem::Texture_UAV(VXGI_CT_COMMON_UAV_SLOT_0),            //
            nvrhi::BindingLayoutItem::Texture_UAV(VXGI_CT_COMMON_UAV_SLOT_1),            //
            nvrhi::BindingLayoutItem::Texture_UAV(VXGI_CT_COMMON_UAV_SLOT_2),            //
            nvrhi::BindingLayoutItem::Sampler(VXGI_CT_POINT_SAMPLER_SLOT),               // s_Point
            nvrhi::BindingLayoutItem::Sampler(VXGI_CT_LINEAR_BORDER_SAMPLER_SLOT),       // SamplerLinearBorder
            nvrhi::BindingLayoutItem::Sampler(VXGI_CT_ENV_MAP_SAMPLER_SLOT),  // s_EnvironmentMapSampler
        };

        m_ComputeBindingLayout = m_Device->createBindingLayout(bindingLayoutDesc);

        bindingLayoutDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
        bindingLayoutDesc.registerSpaceIsDescriptorSet = true;
        bindingLayoutDesc.bindings = {
            nvrhi::BindingLayoutItem::PushConstants(0, 3 * sizeof(float)),  // FullScreenQuadCB
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(
                VXGI_CT_BUILTIN_CBV_SLOT),                                          // cBuiltinTracingParameters
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_DEPTH_BUFFER_SRV_SLOT),   // g_DepthBuffer
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_NORMAL_BUFFER_SRV_SLOT),  // g_TargetNormal
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_FLAT_NORMAL_BUFFER_SRV_SLOT),  // g_TargetFlatNormal
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_STENCIL_BUFFER_SRV_SLOT),      // g_TargetStencil
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_PREV_DEPTH_BUFFER_SRV_SLOT),   // g_PrevDepthBuffer
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_PREV_NORMAL_BUFFER_SRV_SLOT),  // g_PrevTargetNormal
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_0),           //
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_1),           //
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_2),           //
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_3),           //
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_4),           //
            nvrhi::BindingLayoutItem::Sampler(VXGI_CT_POINT_SAMPLER_SLOT),               // s_Point
            nvrhi::BindingLayoutItem::Sampler(VXGI_CT_LINEAR_BORDER_SAMPLER_SLOT),       // SamplerLinearBorder
            nvrhi::BindingLayoutItem::Sampler(VXGI_CT_ENV_MAP_SAMPLER_SLOT),  // s_EnvironmentMapSampler
        };
        m_GraphicsBindingLayout = m_Device->createBindingLayout(bindingLayoutDesc);

        bindingLayoutDesc.bindings = {
            nvrhi::BindingLayoutItem::PushConstants(0, 3 * sizeof(float)),  // FullScreenQuadCB
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(
                VXGI_CT_BUILTIN_CBV_SLOT),                                          // cBuiltinTracingParameters
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_DEPTH_BUFFER_SRV_SLOT),   // g_DepthBuffer
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_NORMAL_BUFFER_SRV_SLOT),  // g_TargetNormal
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_FLAT_NORMAL_BUFFER_SRV_SLOT),  // g_TargetFlatNormal
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_STENCIL_BUFFER_SRV_SLOT),      // g_TargetStencil
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_PREV_DEPTH_BUFFER_SRV_SLOT),   // g_PrevDepthBuffer
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_PREV_NORMAL_BUFFER_SRV_SLOT),  // g_PrevTargetNormal
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_0),           //
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_1),           //
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_2),           //
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_3),           //
            nvrhi::BindingLayoutItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_4),           //
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(VXGI_CT_SAMPLE_POSITIONS_UAV_SLOT),    //
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(VXGI_CT_CONE_DIRECTIONS_UAV_SLOT),     //
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(VXGI_CT_APPEND_COUNTERS_UAV_SLOT),     //
            nvrhi::BindingLayoutItem::Sampler(VXGI_CT_POINT_SAMPLER_SLOT),               // s_Point
            nvrhi::BindingLayoutItem::Sampler(VXGI_CT_LINEAR_BORDER_SAMPLER_SLOT),       // SamplerLinearBorder
            nvrhi::BindingLayoutItem::Sampler(VXGI_CT_ENV_MAP_SAMPLER_SLOT),  // s_EnvironmentMapSampler
        };

        m_GraphicsBindingLayoutDebug = m_Device->createBindingLayout(bindingLayoutDesc);

        bindingLayoutDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
        bindingLayoutDesc.registerSpaceIsDescriptorSet = true;
        bindingLayoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),  // VisualizeConstants
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),    // t_SamplePositions
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),    // t_ConeDirections
            nvrhi::BindingLayoutItem::Texture_SRV(2),             // t_EmittanceDebug
        };
        m_pVisualizeBindingLayout = m_Device->createBindingLayout(bindingLayoutDesc);
    }

    return Status::OK;
}

Status ViewTracer::LoadTracingShaders(int sparsity, bool gbufferMSAA) {
    if (sparsity != m_PreviousTracingShaderSparsity && sparsity > 0) {
        m_ShaderReloadRequired = 1;
        m_PreviousTracingShaderSparsity = sparsity;
    }
    if (!m_PreviousTracingShaderSparsity) {
        m_ShaderReloadRequired = 1;
        m_PreviousTracingShaderSparsity = 1;
    }
    if (gbufferMSAA != m_PreviousFrameGBufferMSAA) {
        m_ShaderReloadRequired = 1;
        m_PreviousFrameGBufferMSAA = gbufferMSAA;
    }

    if (!m_ShaderReloadRequired)
        return Status::OK;

    ReleaseTracingShaders();

    auto TracingBindingLayout = m_Parent->GetVoxelTexture()->GetTracingBindingLayout();
    nvrhi::ShaderHandle cs;
    nvrhi::ComputePipelineDesc csoDesc;
    csoDesc.bindingLayouts = {m_ComputeBindingLayout, TracingBindingLayout};

    donut::engine::ShaderMacro MSAA_G_Buffer_Macro{"MSAA_G_BUFFER", gbufferMSAA ? "1" : "0"};
    donut::engine::ShaderMacro Ambient_Occlusion_Mode_Macro{"AMBIENT_OCCLUSION_MODE",
                                                            m_AmbientOcclusionMode ? "1" : "0"};
    {
        std::vector<donut::engine::ShaderMacro> DiffuseTracingPS_Macros;
        auto VoxelTexture = m_Parent->GetVoxelTexture();
        auto EmittanceFormatMacrosForSampling = GetEmittanceFormatMacroForSampling();
        DiffuseTracingPS_Macros.push_back(EmittanceFormatMacrosForSampling);
        DiffuseTracingPS_Macros.emplace_back("SPARSE_TRACING",
                                             m_PreviousTracingShaderSparsity <= 1 ? "0" : "1");
        DiffuseTracingPS_Macros.emplace_back("USE_SAVE_SAMPLES", "0");
        DiffuseTracingPS_Macros.push_back(MSAA_G_Buffer_Macro);
        m_pDiffuseTracingPS = m_ShaderFactory->CreateShader("app/ConeTracing/DiffuseTracingPS.hlsl", "main",
                                                            &DiffuseTracingPS_Macros, nvrhi::ShaderType::Pixel);
        NVRHI_ASSERT(m_pDiffuseTracingPS);

        DiffuseTracingPS_Macros[2].definition = "1";
        m_pDiffuseTracingDebugPS =
            m_ShaderFactory->CreateShader("app/ConeTracing/DiffuseTracingPS.hlsl", "main",
                                          &DiffuseTracingPS_Macros, nvrhi::ShaderType::Pixel);
        NVRHI_ASSERT(m_pDiffuseTracingDebugPS);

        if (m_PreviousTracingShaderSparsity > 1) {
            nvrhi::GraphicsPipelineDesc psoDesc;
            nvrhi::ShaderHandle ps;
            nvrhi::FramebufferInfo fbInfo;

            auto pRefinementGridVS = m_ShaderFactory->CreateShader("app/ConeTracing/RefinementGridVS.hlsl",
                                                                   "main", nullptr, nvrhi::ShaderType::Vertex);
            NVRHI_ASSERT(pRefinementGridVS);

            psoDesc.primType = nvrhi::PrimitiveType::TriangleList;
            psoDesc.VS = pRefinementGridVS;

            ps = m_ShaderFactory->CreateShader("app/ConeTracing/CopyToStencilPS.hlsl", "main", nullptr,
                                               nvrhi::ShaderType::Pixel);
            NVRHI_ASSERT(ps);
            psoDesc.PS = ps;
            psoDesc.bindingLayouts = {m_GraphicsBindingLayout};

            psoDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Back;
            psoDesc.renderState.rasterState.scissorEnable = 1;
            psoDesc.renderState.depthStencilState.depthTestEnable = false;
            psoDesc.renderState.depthStencilState.stencilEnable = true;
            psoDesc.renderState.depthStencilState.stencilReadMask = 0;
            psoDesc.renderState.depthStencilState.stencilWriteMask = 1;
            psoDesc.renderState.depthStencilState.stencilRefValue = 1;
            psoDesc.renderState.depthStencilState.frontFaceStencil.passOp = nvrhi::StencilOp::Replace;
            psoDesc.renderState.depthStencilState.backFaceStencil.passOp = nvrhi::StencilOp::Replace;

            fbInfo = {};
            fbInfo.setDepthFormat(m_AvailableDSFormat);

            m_pCopyToStencilPS = m_Device->createGraphicsPipeline(psoDesc, fbInfo);
            NVRHI_ASSERT(m_pCopyToStencilPS);

            DiffuseTracingPS_Macros[1].definition = "0";
            DiffuseTracingPS_Macros[2].definition = "0";
            ps = m_ShaderFactory->CreateShader("app/ConeTracing/DiffuseTracingPS.hlsl", "main",
                                               &DiffuseTracingPS_Macros, nvrhi::ShaderType::Pixel);
            NVRHI_ASSERT(ps);
            psoDesc.PS = ps;
            psoDesc.bindingLayouts = {m_GraphicsBindingLayout, TracingBindingLayout};

            psoDesc.renderState.depthStencilState.stencilReadMask = 1;
            psoDesc.renderState.depthStencilState.stencilWriteMask = 0;
            psoDesc.renderState.depthStencilState.stencilRefValue = 1;
            psoDesc.renderState.depthStencilState.frontFaceStencil.stencilFunc = nvrhi::ComparisonFunc::Equal;
            psoDesc.renderState.depthStencilState.backFaceStencil.stencilFunc = nvrhi::ComparisonFunc::Equal;

            fbInfo = {};
            fbInfo.addColorFormat(m_AmbientOcclusionMode ? nvrhi::Format::RGBA8_UNORM
                                                         : nvrhi::Format::RGBA16_FLOAT);
            fbInfo.setDepthFormat(m_AvailableDSFormat);

            m_pDiffuseTracingRefinePS = m_Device->createGraphicsPipeline(psoDesc, fbInfo);
            NVRHI_ASSERT(m_pDiffuseTracingRefinePS);
        }
    }

    if (!m_AmbientOcclusionMode) {
        std::vector<donut::engine::ShaderMacro> SpecularTracingPS_Macros;
        auto VoxelTexture = m_Parent->GetVoxelTexture();
        SpecularTracingPS_Macros.push_back(GetEmittanceFormatMacroForSampling());
        SpecularTracingPS_Macros.emplace_back("USE_SAVE_SAMPLES", "0");
        SpecularTracingPS_Macros.push_back(MSAA_G_Buffer_Macro);

        nvrhi::GraphicsPipelineDesc psoDesc;
        nvrhi::FramebufferInfo fbInfo;
        psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        psoDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        psoDesc.renderState.depthStencilState.depthTestEnable = false;
        psoDesc.renderState.rasterState.depthClipEnable = true;
        psoDesc.VS = m_Parent->GetFullScreenQuadVS();

        auto ps = m_ShaderFactory->CreateShader("app/ConeTracing/SpecularTracingPS.hlsl", "main",
                                                &SpecularTracingPS_Macros, nvrhi::ShaderType::Pixel);
        NVRHI_ASSERT(ps);
        psoDesc.PS = ps;
        psoDesc.bindingLayouts = {m_GraphicsBindingLayout, TracingBindingLayout};
        fbInfo.addColorFormat(nvrhi::Format::RGBA16_FLOAT);
        m_SpecularTracingPSOs[0] = m_Device->createGraphicsPipeline(psoDesc, fbInfo);
        NVRHI_ASSERT(m_SpecularTracingPSOs[0]);

        SpecularTracingPS_Macros[1].definition = "1";
        ps = m_ShaderFactory->CreateShader("app/ConeTracing/SpecularTracingPS.hlsl", "main",
                                           &SpecularTracingPS_Macros, nvrhi::ShaderType::Pixel);
        NVRHI_ASSERT(ps);
        psoDesc.PS = ps;
        psoDesc.bindingLayouts = {m_GraphicsBindingLayoutDebug, TracingBindingLayout};
        m_SpecularTracingPSOs[1] = m_Device->createGraphicsPipeline(psoDesc, fbInfo);
        NVRHI_ASSERT(m_SpecularTracingPSOs[1]);

        std::vector<donut::engine::ShaderMacro> FilterSpecularCS_Macros{MSAA_G_Buffer_Macro};
        cs = m_ShaderFactory->CreateShader("app/ConeTracing/SpecularFilteringCS.hlsl", "main",
                                           &FilterSpecularCS_Macros, nvrhi::ShaderType::Compute);
        NVRHI_ASSERT(cs);
        csoDesc.CS = cs;

        csoDesc.bindingLayouts = {m_ComputeBindingLayout, TracingBindingLayout};

        m_pFilterSpecularCS = m_Device->createComputePipeline(csoDesc);
        NVRHI_ASSERT(m_pFilterSpecularCS);
    }

    if (m_PreviousTracingShaderSparsity > 1) {
        std::vector<donut::engine::ShaderMacro> InterpolationPS_Macros;
        InterpolationPS_Macros.push_back(Ambient_Occlusion_Mode_Macro);
        InterpolationPS_Macros.push_back(MSAA_G_Buffer_Macro);
        InterpolationPS_Macros.emplace_back("DOWNSAMPLE_SCALE",
                                            std::to_string(m_PreviousTracingShaderSparsity));
        cs = m_ShaderFactory->CreateShader("app/ConeTracing/DiffuseInterpolationCS.hlsl", "main",
                                           &InterpolationPS_Macros, nvrhi::ShaderType::Compute);
        NVRHI_ASSERT(cs);
        csoDesc.CS = cs;
        csoDesc.bindingLayouts = {m_ComputeBindingLayout, TracingBindingLayout};
        m_pInterpolateIlluminationCS = m_Device->createComputePipeline(csoDesc);
        NVRHI_ASSERT(m_pInterpolateIlluminationCS);
    }

    if (m_PreviousTracingShaderSparsity > 1) {
        std::vector<donut::engine::ShaderMacro> InterpolateCoarseCS_Macros;
        InterpolateCoarseCS_Macros.push_back(Ambient_Occlusion_Mode_Macro);
        InterpolateCoarseCS_Macros.push_back(MSAA_G_Buffer_Macro);

        cs = m_ShaderFactory->CreateShader("app/ConeTracing/DiffuseFilteringCS.hlsl", "main",
                                           &InterpolateCoarseCS_Macros, nvrhi::ShaderType::Compute);
        NVRHI_ASSERT(cs);
        csoDesc.CS = cs;
        csoDesc.bindingLayouts = {m_ComputeBindingLayout, TracingBindingLayout};
        m_pInterpolateCoarseCS = m_Device->createComputePipeline(csoDesc);
        NVRHI_ASSERT(m_pInterpolateCoarseCS);
    }

    if (!m_AmbientOcclusionMode) {
        std::vector<donut::engine::ShaderMacro> TraceVisionPS_Macros;
        auto VoxelTexture = m_Parent->GetVoxelTexture();
        TraceVisionPS_Macros.push_back(GetEmittanceFormatMacroForSampling());
        TraceVisionPS_Macros.emplace_back("USE_SAVE_SAMPLES", "0");
        TraceVisionPS_Macros.push_back(MSAA_G_Buffer_Macro);

        m_pTracerVisionPS = m_ShaderFactory->CreateShader("app/ConeTracing/TracerVisionPS.hlsl", "main",
                                                          &TraceVisionPS_Macros, nvrhi::ShaderType::Pixel);
        NVRHI_ASSERT(m_pTracerVisionPS);

        TraceVisionPS_Macros[1].definition = "1";
        m_pTracerVisionDebugPS = m_ShaderFactory->CreateShader("app/ConeTracing/TracerVisionPS.hlsl", "main",
                                                               &TraceVisionPS_Macros, nvrhi::ShaderType::Pixel);
        NVRHI_ASSERT(m_pTracerVisionDebugPS);
    }

    {
        std::vector<donut::engine::ShaderMacro> GBuffer_Macros{MSAA_G_Buffer_Macro};

        cs = m_ShaderFactory->CreateShader("app/ConeTracing/ComputeScreenSpaceOcclusionCS.hlsl", "main",
                                           &GBuffer_Macros, nvrhi::ShaderType::Compute);
        NVRHI_ASSERT(cs);
        csoDesc.CS = cs;
        csoDesc.bindingLayouts = {m_ComputeBindingLayout};
        m_pComputeScreenSpaceOcclusionCS = m_Device->createComputePipeline(csoDesc);

        cs = m_ShaderFactory->CreateShader("app/ConeTracing/DeinterleaveDepthCS.hlsl", "main", &GBuffer_Macros,
                                           nvrhi::ShaderType::Compute);
        NVRHI_ASSERT(cs);
        csoDesc.CS = cs;
        csoDesc.bindingLayouts = {m_ComputeBindingLayout};
        m_pDeinterleaveDepthCS = m_Device->createComputePipeline(csoDesc);

        cs = m_ShaderFactory->CreateShader("app/ConeTracing/BlurScreenSpaceOcclusionCS.hlsl", "main", nullptr,
                                           nvrhi::ShaderType::Compute);
        NVRHI_ASSERT(cs);
        csoDesc.CS = cs;
        csoDesc.bindingLayouts = {m_ComputeBindingLayout};
        m_pBlurScreenSpaceOcclusionCS = m_Device->createComputePipeline(csoDesc);
    }

    m_ShaderReloadRequired = false;

    return Status::OK;
}

void ViewTracer::ReleaseTracingShaders() {
    m_pDiffuseTracingPS = nullptr;
    m_pDiffuseTracingDebugPS = nullptr;
    m_pCopyToStencilPS = nullptr;
    m_pDiffuseTracingRefinePS = nullptr;
    m_SpecularTracingPSOs[0] = nullptr;
    m_SpecularTracingPSOs[1] = nullptr;
    m_pFilterSpecularCS = nullptr;
    m_pInterpolateIlluminationCS = nullptr;
    m_pInterpolateCoarseCS = nullptr;
    m_pTracerVisionPS = nullptr;
    m_pTracerVisionDebugPS = nullptr;
    m_pComputeScreenSpaceOcclusionCS = nullptr;
    m_pDeinterleaveDepthCS = nullptr;
    m_pBlurScreenSpaceOcclusionCS = nullptr;

    for (int i = 0; i < dim(m_DiffuseTracingPSOs); ++i) {
        for (int j = 0; j < dim(m_DiffuseTracingPSOs[i]); ++j)
            m_DiffuseTracingPSOs[i][j] = nullptr;
    }

    m_TracerVisionFramebuffer = nullptr;
    m_TracerVisionPSOs[0] = nullptr;
    m_TracerVisionPSOs[1] = nullptr;
}

void ViewTracer::setPixelToSave(nvrhi::ICommandList *commandList, int x, int y) {
    m_PixelToSave = {x, y, 0};
    if (m_PixelToSave.x >= 0 && m_PixelToSave.y >= 0)
        commandList->clearBufferUInt(m_AppendCounters, 0);
}

float ViewTracer::getDiffuseConeAngle(int numCones) {
    numCones = std::max(numCones, 1);

    float angle = dm::degrees(2.f * std::acos(1.f - 1.f / float(numCones)));
    if (angle > 60.f)
        angle = 60.f;
    return angle;
}

void ViewTracer::FillTracingConstants(BuiltinTracingConstants *builtinConstants,
                                      const CommonTracingParameters &params,
                                      const ViewTracerInputBuffers *inputBuffers,
                                      const ViewTracerInputBuffers *inputBuffersPreviousFrame,
                                      const uint2 &gbufferTextureSize) {
    auto geometry = m_Parent->GetClipmapGeometry();
    memset(builtinConstants, 0, sizeof(*builtinConstants));
    FillGBufferConstants(&builtinConstants->m_GBuffer, inputBuffers, gbufferTextureSize);

    if (inputBuffersPreviousFrame) {
        FillGBufferConstants(&builtinConstants->m_PreviousGBufer, inputBuffersPreviousFrame,
                             gbufferTextureSize);

        builtinConstants->m_ReprojectionMatrix = builtinConstants->m_GBuffer.viewProjMatrixInv *
                                                 inputBuffersPreviousFrame->viewMatrix *
                                                 inputBuffersPreviousFrame->projMatrix;
    }

    builtinConstants->m_nMaxSamples = params.maxSamples;
    builtinConstants->m_bFlipOpacityDirections = params.flipOpacityDirections;
    builtinConstants->m_vDebugParams = params.debugParameters;
    builtinConstants->m_vPixelToSave = m_PixelToSave;
    builtinConstants->m_vGridOrigin = float2(std::ceil(builtinConstants->m_GBuffer.viewportOrigin.x -
                                                       builtinConstants->m_GBuffer.firstSamplePosition.x),
                                             std::ceil(builtinConstants->m_GBuffer.viewportOrigin.y -
                                                       builtinConstants->m_GBuffer.firstSamplePosition.y));
    builtinConstants->m_fTracingStep = params.tracingStep;
    builtinConstants->m_fOpacityCorrectionFactor = params.opacityCorrectionFactor;

    if (m_PixelToSave.x >= 0 && m_PixelToSave.y >= 0) {
        m_ClipmapCenterAtSampleSave = float4(geometry->m_Center, 1.f / geometry->m_VoxelSizes[0]);
    }
}

void ViewTracer::FillGBufferConstants(GBufferParameters *params, const ViewTracerInputBuffers *inputBuffers,
                                      const uint2 &gbufferTextureSize) {
    params->viewProjMatrix = inputBuffers->viewMatrix * inputBuffers->projMatrix;
    params->viewProjMatrixInv = inverse(params->viewProjMatrix);
    params->viewMatrix = inputBuffers->viewMatrix;
    params->cameraPosition = inverse(inputBuffers->viewMatrix).row3;
    params->gbufferSize = float2(gbufferTextureSize);
    params->gbufferSizeInv = 1.f / params->gbufferSize;
    params->viewportOrigin = float2(inputBuffers->gbufferViewport.minX, inputBuffers->gbufferViewport.minY);
    params->viewportSize =
        float2(inputBuffers->gbufferViewport.width(), inputBuffers->gbufferViewport.height());
    params->viewportSizeInv = 1.f / params->viewportSize;
    params->firstSamplePosition = float2(0.5f);
    params->projectionA = inputBuffers->projMatrix.row2.z / inputBuffers->projMatrix.row2.w;
    params->projectionB = inputBuffers->projMatrix.row3.z / inputBuffers->projMatrix.row2.w;
    params->radiusToScreen =
        0.5f * inputBuffers->gbufferViewport.height() * std::abs(inputBuffers->projMatrix.row1.y);
    params->uvToView =
        float4(2.f * inputBuffers->projMatrix.row2.w / inputBuffers->projMatrix.row0.x,
               -2.f * inputBuffers->projMatrix.row2.w / inputBuffers->projMatrix.row1.y, -1.f, 1.f);

    float minDepth = inputBuffers->gbufferViewport.minZ;
    float depthRange = inputBuffers->gbufferViewport.maxZ - minDepth;
    params->depthScale = 1.f / depthRange;
    params->depthBias = -minDepth / depthRange;
    params->normalScale = inputBuffers->gbufferNormalScale;
    params->normalBias = inputBuffers->gbufferNormalBias;
}

void ViewTracer::UpdateConeVectors(nvrhi::ICommandList *commandList, uint numCones, bool enableConeRotation,
                                   bool enableRandomConeOffsets, float coneNormalGroupingFactor) {
    if (m_PreviousDiffuseTracingParams.numCones != numCones ||
        m_PreviousDiffuseTracingParams.coneNormalGroupingFactor != coneNormalGroupingFactor ||
        m_PreviousDiffuseTracingParams.enableConeRotation != enableConeRotation ||
        m_PreviousDiffuseTracingParams.enableRandomConeOffsets != enableRandomConeOffsets) {
        m_PreviousDiffuseTracingParams.numCones = numCones;
        m_PreviousDiffuseTracingParams.coneNormalGroupingFactor = coneNormalGroupingFactor;
        m_PreviousDiffuseTracingParams.enableConeRotation = enableConeRotation;
        m_PreviousDiffuseTracingParams.enableRandomConeOffsets = enableRandomConeOffsets;

        std::vector<float4> coneVectors((size_t)numCones);
        coneVectors[0] = float4(0.f, 1.f, 0.f, 0.f);

        m_SumOfConeDotNormals = 1.f;

        for (uint cone = 1; cone < numCones; ++cone) {
            float t = 0.f;
            float pos_4 = 0.5f;
            for (uint kk = cone; kk; kk >>= 1) {
                if (kk & 1)
                    t = t + pos_4;
                pos_4 = pos_4 * 0.5f;
            }

            float normalFactor = dm::saturate(coneNormalGroupingFactor);
            float ta = dm::lerp(t, 1.f, normalFactor * 0.8f + 0.1f);
            float st = std::sqrt(1.f - ta * ta);

            float3 n = {st, ta, (float(cone) + 0.5f) / float(numCones - 1) * 2.f * dm::PI_f};

            coneVectors[cone] = float4(n, 0.f);
            m_SumOfConeDotNormals += ta;
        }

        const uint numRandoms = 16;
        std::vector<std::vector<float4>> randomizedConeVectors((size_t)numRandoms);

        for (uint nrand = 0; nrand < numRandoms; ++nrand) {
            float randRotation = (enableConeRotation ? g_RandomValuesSmall[nrand] : 0.5f) * 2.f * PI_f;
            float randOffset = enableRandomConeOffsets ? g_RandomValuesSmall[nrand] : 0.5f;

            for (uint cone = 0; cone < numCones; ++cone) {
                float3 coneData = coneVectors[cone];

                float4 tangentSpace = {std::cos(coneData.z + randRotation) * coneData.x, coneData.y,
                                       std::sin(coneData.z + randRotation) * coneData.x, randOffset};

                randomizedConeVectors[nrand].push_back(tangentSpace);
            }
        }

        nvrhi::TextureDesc coneDirectionsTexDesc;
        coneDirectionsTexDesc.format = nvrhi::Format::RGBA32_FLOAT;
        coneDirectionsTexDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
        coneDirectionsTexDesc.width = 4;
        coneDirectionsTexDesc.height = 4;
        coneDirectionsTexDesc.arraySize = numCones;
        coneDirectionsTexDesc.debugName = "coneDirectionsStagingTexture";
        auto tilesOfConesBuffer =
            m_Device->createStagingTexture(coneDirectionsTexDesc, nvrhi::CpuAccessMode::Write);

        coneDirectionsTexDesc.debugName = "coneDirectionsTexture";
        m_ConeDirectionsTexture = m_Device->createTexture(coneDirectionsTexDesc);

        commandList->beginTrackingTextureState(m_ConeDirectionsTexture, nvrhi::AllSubresources,
                                               nvrhi::ResourceStates::CopyDest);

        for (uint i = 0; i < numCones; ++i) {
            size_t rowPitch;
            auto sliceRegion = nvrhi::TextureSlice{}.setArraySlice(i);
            auto tilesOfCones = (float4 *)m_Device->mapStagingTexture(tilesOfConesBuffer, sliceRegion,
                                                                      nvrhi::CpuAccessMode::Write, &rowPitch);
            NVRHI_ASSERT(tilesOfCones);
            NVRHI_ASSERT((rowPitch & (sizeof(float4) - 1)) == 0);
            int N = rowPitch / sizeof(float4);

            float4 basicCone = randomizedConeVectors[0][i];
            tilesOfCones[0] = basicCone;

            for (uint j = 1; j < numRandoms; ++j) {
                auto &vectors = randomizedConeVectors[j];
                float bestDistance = 1000.f;
                auto bestElement = vectors.end();
                for (auto candidate = vectors.begin(); candidate != vectors.end(); ++candidate) {
                    float distance = lengthSquared(float3(basicCone) - float3(*candidate));
                    if (distance < bestDistance) {
                        bestDistance = distance;
                        bestElement = candidate;
                    }
                }

                int ii = j / 4;
                int jj = j - ii * 4;
                tilesOfCones[N * ii + jj] = *bestElement;
                vectors.erase(bestElement);
            }

            m_Device->unmapStagingTexture(tilesOfConesBuffer);

            commandList->copyTexture(m_ConeDirectionsTexture, sliceRegion, tilesOfConesBuffer, sliceRegion);
        }

        commandList->setPermanentTextureState(m_ConeDirectionsTexture, nvrhi::ResourceStates::ShaderResource);
    }
}

Status ViewTracer::ValidateTextureSize(TextureHandleVerbose *texture, uint width, uint height, uint arraySize,
                                       nvrhi::Format format, bool *invalidated) {
    *invalidated = false;

    if (texture->value) {
        auto &desc = texture->value->getDesc();
        if (desc.width != width || desc.height != height || desc.arraySize != arraySize) {
            texture->value = nullptr;
            *invalidated = true;
        }
    }

    if (!texture->value && width && height) {
        nvrhi::TextureDesc desc;
        desc.dimension =
            arraySize > 1 ? nvrhi::TextureDimension::Texture2DArray : nvrhi::TextureDimension::Texture2D;
        desc.width = width;
        desc.height = height;
        desc.arraySize = arraySize;
        desc.isRenderTarget = true;
        desc.format = format;
        desc.mipLevels = 1;
        desc.isUAV = format != m_AvailableDSFormat;
        desc.isTypeless = format == m_AvailableDSFormat ? true : false;
        desc.initialState = format == m_AvailableDSFormat ? nvrhi::ResourceStates::DepthWrite
                                                          : nvrhi::ResourceStates::RenderTarget;
        desc.keepInitialState = true;
        texture->value = m_Device->createTexture(desc);
        if (!texture->value)
            return Status::RESOURCE_CREATION_FAILED;
        *invalidated = true;
    }

    return Status::OK;
}

Status ViewTracer::ValidateDownsampledTargets(uint width, uint height, bool useFiltering) {
    Status rc = Status::OK;
    bool invalidated;
    for (uint n = 0; n < m_DownsampledTargetCount; ++n) {
        if (VXGI_FAILED(rc = ValidateTextureSize(&m_DownsampledTargets[n], width, height, 1,
                                                 nvrhi::Format::RGBA16_FLOAT, &invalidated)))
            return rc;
        if (n == 0 && invalidated)
            m_DownsampledFb = nullptr;

        if (useFiltering) {
            if (VXGI_FAILED(rc = ValidateTextureSize(&m_DownsampledSmoothedTargets[n], width, height, 1,
                                                     nvrhi::Format::RGBA16_FLOAT, &invalidated)))
                return rc;
        } else {
            if (VXGI_FAILED(rc = ValidateTextureSize(&m_DownsampledSmoothedTargets[n], 0, height, 1,
                                                     nvrhi::Format::RGBA16_FLOAT, &invalidated)))
                return rc;
        }
        if (n == 0 && invalidated)
            m_DownsampledSmoothedFb = nullptr;
    }

    if (m_DownsampledTargets[0].value && !m_DownsampledFb) {
        nvrhi::FramebufferDesc fbDesc;
        for (uint n = 0; n < m_DownsampledTargetCount; ++n)
            fbDesc.addColorAttachment(m_DownsampledTargets[n].value);
        m_DownsampledFb = m_Device->createFramebuffer(fbDesc);
        if (!m_DownsampledFb)
            return Status::RESOURCE_CREATION_FAILED;
    }

    if (m_DownsampledSmoothedTargets[0].value && !m_DownsampledSmoothedFb) {
        nvrhi::FramebufferDesc fbDesc;
        for (uint n = 0; n < m_DownsampledTargetCount; ++n)
            fbDesc.addColorAttachment(m_DownsampledSmoothedTargets[n].value);
        m_DownsampledSmoothedFb = m_Device->createFramebuffer(fbDesc);
        if (!m_DownsampledSmoothedFb)
            return Status::RESOURCE_CREATION_FAILED;
    }

    return rc;
}

Status ViewTracer::ValidateSpecularTargets(uint width, uint height, bool useFiltering) {
    Status rc = Status::OK;
    bool invalidated;
    for (uint n = 0; n < 2; ++n) {
        if (VXGI_FAILED(rc = ValidateTextureSize(&m_SpecularTargets[n], width, height, 1,
                                                 nvrhi::Format::RGBA16_FLOAT, &invalidated)))
            return rc;
        if (invalidated) {
            m_SpecularTracingFbs[n] = nullptr;
            if (m_SpecularTargets[n].value) {
                nvrhi::FramebufferDesc fbDesc;
                fbDesc.addColorAttachment(m_SpecularTargets[n].value);
                m_SpecularTracingFbs[n] = m_Device->createFramebuffer(fbDesc);
                if (!m_SpecularTracingFbs[n])
                    return Status::RESOURCE_CREATION_FAILED;
            }
        }
    }

    if (useFiltering) {
        if (VXGI_FAILED(rc = ValidateTextureSize(&m_FilteredSpecularTarget, width, height, 1,
                                                 nvrhi::Format::RGBA16_FLOAT, &invalidated)))
            return rc;
    } else {
        if (VXGI_FAILED(rc = ValidateTextureSize(&m_FilteredSpecularTarget, 0, height, 1,
                                                 nvrhi::Format::RGBA16_FLOAT, &invalidated)))
            return rc;
    }

    return rc;
}

Status ViewTracer::ValidateInterpolatedTargets(uint width, uint height) {
    Status rc = Status::OK;
    bool invalidated;
    for (uint n = 0; n < 2; ++n) {
        if (m_AmbientOcclusionMode)
            rc = ValidateTextureSize(&m_InterpolatedTargets[n], width, height, 1, nvrhi::Format::RGBA8_UNORM,
                                     &invalidated);
        else
            rc = ValidateTextureSize(&m_InterpolatedTargets[n], width, height, 1, nvrhi::Format::RGBA16_FLOAT,
                                     &invalidated);

        if (VXGI_FAILED(rc))
            return rc;

        if (invalidated) {
            m_InterpolatedFbs[n] = nullptr;
            if (m_InterpolatedTargets[n].value) {
                nvrhi::FramebufferDesc fbDesc;
                fbDesc.addColorAttachment(m_InterpolatedTargets[0].value);
                m_InterpolatedFbs[n] = m_Device->createFramebuffer(fbDesc);
                if (!m_InterpolatedFbs[n])
                    return Status::RESOURCE_CREATION_FAILED;
            }
        }
    }

    if (VXGI_FAILED(rc = ValidateTextureSize(&m_RefinementControlTexture, width, height, 1,
                                             nvrhi::Format::R8_UNORM, &invalidated)))
        return rc;

    if (VXGI_FAILED(rc = ValidateTextureSize(&m_RefinementStencilTexture, width, height, 1, m_AvailableDSFormat,
                                             &invalidated)))
        return rc;
    if (invalidated) {
        m_RefinementStencilFb = nullptr;
        if (m_RefinementStencilTexture.value) {
            nvrhi::FramebufferDesc fbDesc;
            fbDesc.setDepthAttachment(m_RefinementStencilTexture.value);
            m_RefinementStencilFb = m_Device->createFramebuffer(fbDesc);
            if (!m_RefinementStencilFb)
                return Status::RESOURCE_CREATION_FAILED;

            fbDesc.colorAttachments.resize(1);
            fbDesc.colorAttachments[0] = {m_InterpolatedTargets[0].value};
            m_DiffuseTracingRefineFbs[0] = m_Device->createFramebuffer(fbDesc);
            if (!m_DiffuseTracingRefineFbs[0])
                return Status::RESOURCE_CREATION_FAILED;

            fbDesc.colorAttachments[0] = {m_InterpolatedTargets[1].value};
            m_DiffuseTracingRefineFbs[1] = m_Device->createFramebuffer(fbDesc);
            if (!m_DiffuseTracingRefineFbs[1])
                return Status::RESOURCE_CREATION_FAILED;
        }
    }

    if (VXGI_FAILED(rc = ValidateTextureSize(&m_RefinementGridTexture, div_ceil(width, 32u),
                                             div_ceil(height, 32u), 1, nvrhi::Format::R8_UNORM, &invalidated)))
        return rc;

    return rc;
}

Status ViewTracer::computeDiffuseChannel(nvrhi::ICommandList *commandList,
                                         const DiffuseTracingParameters &params, nvrhi::ITexture **outDiffuse,
                                         const ViewTracerInputBuffers *inputBuffers,
                                         const ViewTracerInputBuffers *inputBuffersPreviousFrame) {
    Status rc = Status::OK;

    auto geometry = m_Parent->GetClipmapGeometry();
    auto VoxelTexture = m_Parent->GetVoxelTexture();
    auto &descDepth = inputBuffers->gbufferDepth->getDesc();
    uint2 textureSize = {descDepth.width, descDepth.height};
    uint sampleCount = descDepth.sampleCount;

    if (inputBuffersPreviousFrame) {
        auto &descDepthPrev = inputBuffersPreviousFrame->gbufferDepth->getDesc();
        uint2 textureSizePrev = uint2{descDepthPrev.width, descDepthPrev.height};

        if (inputBuffersPreviousFrame->gbufferDepth == inputBuffers->gbufferDepth &&
                !params.enableReprojectionFromSameFrame ||
            any(textureSizePrev != textureSize) ||
            VXGI_FAILED(SortDiffuseRenderTargetsForTR(inputBuffersPreviousFrame->gbufferDepth))) {
            inputBuffersPreviousFrame = nullptr;
        }
    }

    m_InterpolatedTargets[0].attachment = inputBuffers->gbufferDepth;

    uint numCones = std::min(params.numCones, 128u);

    uint traceSparsity = std::max(1u, params.tracingSparsity);
    uint sparsity = std::min(traceSparsity, 4u);

    if (VXGI_FAILED(rc = LoadTracingShaders(sparsity, sampleCount > 1)))
        return rc;

    int2 viewportSize{(int)(inputBuffers->gbufferViewport.maxX - inputBuffers->gbufferViewport.minX),
                      (int)(inputBuffers->gbufferViewport.maxY - inputBuffers->gbufferViewport.minY)};
    float coneAngleDegrees = params.coneAngle;
    if (params.autoConeAngle)
        coneAngleDegrees = getDiffuseConeAngle(params.numCones);

    BuiltinTracingConstants builtinConstants = {};
    uint2 gbufferTextureSize = textureSize;
    FillTracingConstants(&builtinConstants, params, inputBuffers, inputBuffersPreviousFrame,
                         gbufferTextureSize);
    builtinConstants.m_fConeFactor = GetConeFactor(coneAngleDegrees);

    builtinConstants.m_fDepthdeltaSign = params.farClipZ > params.nearClipZ ? 1.f : -1.f;
    builtinConstants.m_fInitialOffsetBias = params.initialOffsetBias;
    builtinConstants.m_fInitialOffsetDistanceFactor = params.initialOffsetDistanceFactor;
    builtinConstants.m_nNumCones = numCones;
    builtinConstants.m_frNumCones = 1.f / float(numCones);
    builtinConstants.m_vDownsampleScale =
        float4((float)sparsity, (float)sparsity, 1.f / (float)sparsity, 1.f / (float)sparsity);
    builtinConstants.m_fEmittanceScale = PI_f / (std::pow(builtinConstants.m_fConeFactor, 2) * (float)numCones);
    builtinConstants.m_fEmittanceScale = builtinConstants.m_fEmittanceScale * params.irradianceScale;

    UpdateConeVectors(commandList, numCones, params.enableConeRotation && sparsity > 1u,
                      params.enableRandomConeOffsets, params.coneNormalGroupingFactor);

    const bool useFiltering = params.enableConeRotation && sparsity > 1u;
    const bool enableTemporalReprojection =
        inputBuffersPreviousFrame && params.enableTemporalReprojection && sparsity > 1u;
    float temporalReprojectionWeight;
    if (enableTemporalReprojection)
        temporalReprojectionWeight = params.temporalReprojectionWeight;
    else
        temporalReprojectionWeight = 0.f;

    builtinConstants.m_fTemporalReprojectionWeight = temporalReprojectionWeight;
    builtinConstants.m_fReprojectionDepthWeightScale =
        1.f / (float)(params.temporalReprojectionMaxDistanceInVoxels * geometry->m_VoxelSizes[0]);
    builtinConstants.m_fReprojectionNormalWeightExponent = params.temporalReprojectionNormalWeightExponent;

    builtinConstants.m_vRandomOffset = enableTemporalReprojection ? int2(rand(), rand()) : int2(0);

    float3 ambientColor;
    if (maxComponent(params.ambientColor) <= 0.f && m_AmbientOcclusionMode) {
        ambientColor = float3(1.f);
    } else
        ambientColor = params.ambientColor;

    builtinConstants.m_vAmbientColor =
        float4(ambientColor * ((float)params.numCones / m_SumOfConeDotNormals), 0.f);
    if (m_RefinementGridTexture.value) {
        auto &refinementGridTextureDesc = m_RefinementGridTexture.value->getDesc();
        builtinConstants.m_vRefinementGridResolution =
            float4(refinementGridTextureDesc.width, refinementGridTextureDesc.height,
                   1.f / refinementGridTextureDesc.width, 1.f / refinementGridTextureDesc.height);
    } else {
        builtinConstants.m_vRefinementGridResolution = float4{0.f};
    }

    builtinConstants.m_fInterpolationWeightThreshold = dm::saturate(params.interpolationWeightThreshold);

    if (inputBuffers->gbufferStencil) {
        builtinConstants.m_nAltSettingsStencilMask = inputBuffers->altSettingsStencilMask;
        builtinConstants.m_nAltSettingsStencilRefValue = inputBuffers->altSettingsStencilRefValue;
    } else {
        builtinConstants.m_nAltSettingsStencilMask = 0;
        builtinConstants.m_nAltSettingsStencilRefValue = 1;
    }

    builtinConstants.m_fAltInitialOffsetBias = params.altInitialOffsetBias;
    builtinConstants.m_fAltInitialOffsetDistanceFactor = params.altInitialOffsetDistanceFactor;
    builtinConstants.m_fAltNormalOffsetFactor = params.altNormalOffsetFactor;
    builtinConstants.m_fAltTracingStep = params.altTracingStep;

    const bool enableSSAO = params.enableSSAO && params.SSAO_RadiusWorld > 0.f && params.SSAO_Scale > 0.f &&
                            params.SSAO_PowerExponent > 0.f;
    builtinConstants.m_fSSAO_SurfaceBias = enableSSAO ? params.SSAO_SurfaceBias : 0.f;
    builtinConstants.m_fSSAO_RadiusWorld = enableSSAO ? params.SSAO_RadiusWorld : 0.f;
    builtinConstants.m_fSSAO_rBackgroundViewDepth =
        (enableSSAO && params.SSAO_BackgroundViewDepth > 0.f) ? 1.f / params.SSAO_BackgroundViewDepth : 0.f;
    builtinConstants.m_fSSAO_CoarseAO = enableSSAO ? params.SSAO_Scale * 2.f : 0.f;
    builtinConstants.m_fSSAO_PowerExponent = enableSSAO ? params.SSAO_PowerExponent : 0.f;

    if (params.environmentMap) {
        builtinConstants.m_fEnvironmentMapTint = float4(params.environmentMapTint, 0.f);
        auto &envMapDesc = params.environmentMap->getDesc();
        builtinConstants.m_fEnvironmentMapResolution = envMapDesc.width;
        builtinConstants.m_fMaxEnvironmentMapMipLevel = std::max<int>(0, envMapDesc.mipLevels - 1);
    } else {
        builtinConstants.m_fEnvironmentMapTint = float4(0.f);
        builtinConstants.m_fEnvironmentMapResolution = 0.f;
        builtinConstants.m_fMaxEnvironmentMapMipLevel = 0.f;
    }

    builtinConstants.m_fAmbientAttenuationFactor = 2.3f / (params.ambientRange / geometry->m_VoxelSizes[0]);
    builtinConstants.m_fNormalOffsetFactor = params.normalOffsetFactor;
    builtinConstants.m_fAmbientScale = params.ambientScale;
    builtinConstants.m_fAmbientBias = params.ambientBias;
    builtinConstants.m_fAmbientPower = params.ambientPower;

    builtinConstants.m_fAmbientDistanceDarkening = -dm::saturate(params.ambientDistanceDarkening);
    builtinConstants.m_vBackgroundColor = float4(params.backgroundColor, 0.f);
    builtinConstants.m_bEnableRefinement = params.enableSparseTracingRefinement;
    commandList->writeBuffer(m_pTracingCB, &builtinConstants, sizeof(builtinConstants));

    auto TracingBindingSet = VoxelTexture->GetBindingSetForIrradianceMapTracing();
    auto bindingCache = VoxelTexture->GetBindingCache();

    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {
        nvrhi::BindingSetItem::PushConstants(0, sizeof(float3)),
        nvrhi::BindingSetItem::ConstantBuffer(VXGI_CT_BUILTIN_CBV_SLOT, m_pTracingCB),
        nvrhi::BindingSetItem::Texture_SRV(VXGI_CT_DEPTH_BUFFER_SRV_SLOT, inputBuffers->gbufferDepth),
        nvrhi::BindingSetItem::Texture_SRV(VXGI_CT_NORMAL_BUFFER_SRV_SLOT, inputBuffers->gbufferNormal),
        nvrhi::BindingSetItem::Texture_SRV(
            VXGI_CT_FLAT_NORMAL_BUFFER_SRV_SLOT,
            inputBuffers->gbufferGeoNormal ? inputBuffers->gbufferGeoNormal : inputBuffers->gbufferNormal),
        nvrhi::BindingSetItem::Texture_SRV(
            VXGI_CT_STENCIL_BUFFER_SRV_SLOT,
            inputBuffers->gbufferStencil ? inputBuffers->gbufferStencil : m_NullDepthStencilTexture.Get()),
        nvrhi::BindingSetItem::Texture_SRV(
            VXGI_CT_PREV_DEPTH_BUFFER_SRV_SLOT,
            inputBuffersPreviousFrame ? inputBuffersPreviousFrame->gbufferDepth : m_NullTexture.Get()),
        nvrhi::BindingSetItem::Texture_SRV(
            VXGI_CT_PREV_NORMAL_BUFFER_SRV_SLOT,
            inputBuffersPreviousFrame ? inputBuffersPreviousFrame->gbufferNormal : m_NullTexture.Get()),
        nvrhi::BindingSetItem::Sampler(VXGI_CT_POINT_SAMPLER_SLOT, m_PointSampler),
        nvrhi::BindingSetItem::Sampler(VXGI_CT_LINEAR_BORDER_SAMPLER_SLOT, m_Parent->GetLinearWrapSampler()),
        nvrhi::BindingSetItem::Sampler(VXGI_CT_ENV_MAP_SAMPLER_SLOT, m_EnvironmentMapSampler)};
    const uint numBindings = bindingSetDesc.bindings.size();
    nvrhi::ComputeState state;
    uint3 numGroups;

    if (sparsity <= 1u) {
        if (VXGI_FAILED(rc = ValidateDownsampledTargets(0, 0, 0)))
            return rc;

        if (VXGI_FAILED(ValidateInterpolatedTargets(textureSize.x, textureSize.y)))
            return rc;
    } else {
        if (VXGI_FAILED(rc = ValidateDownsampledTargets(div_ceil(textureSize.x, sparsity),
                                                        div_ceil(textureSize.y, sparsity), useFiltering)))
            return rc;

        if (VXGI_FAILED(rc = ValidateInterpolatedTargets(textureSize.x, textureSize.y)))
            return rc;
    }

    if (enableSSAO) {
        bool invalidated;
        if (VXGI_FAILED(rc = ValidateTextureSize(&m_DeinterleavedDepthTextureArray, div_ceil(textureSize.x, 4u),
                                                 div_ceil(textureSize.y, 4u), 16, nvrhi::Format::R32_FLOAT,
                                                 &invalidated)))
            return rc;

        if (VXGI_FAILED(rc = ValidateTextureSize(&m_ScreenSpaceOcclusionTextureArray,
                                                 div_ceil(textureSize.x, 4u), div_ceil(textureSize.y, 4u), 16,
                                                 nvrhi::Format::R8_UNORM, &invalidated)))
            return rc;

        if (VXGI_FAILED(rc = ValidateTextureSize(&m_ScreenSpaceOcclusionBlurTexture, textureSize.x,
                                                 textureSize.y, 1, nvrhi::Format::R8_UNORM, &invalidated)))
            return rc;

        commandList->beginMarker("Tracing: SSAO");
        {
            state.pipeline = m_pDeinterleaveDepthCS;
            bindingSetDesc.bindings.resize(numBindings);
            bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(
                VXGI_CT_COMMON_UAV_SLOT_0, m_DeinterleavedDepthTextureArray.value));
            state.bindings = {bindingCache->GetOrCreateBindingSet(bindingSetDesc, m_ComputeBindingLayout)};
            commandList->setComputeState(state);

            auto &texDesc = m_DeinterleavedDepthTextureArray.value->getDesc();
            numGroups.x = div_ceil(texDesc.width, 8u);
            numGroups.y = div_ceil(texDesc.height, 8u);
            commandList->dispatch(numGroups.x, numGroups.y);
        }

        {
            state.pipeline = m_pComputeScreenSpaceOcclusionCS;
            bindingSetDesc.bindings.resize(numBindings);
            bindingSetDesc.bindings.insert(
                bindingSetDesc.bindings.end(),
                {nvrhi::BindingSetItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_1, m_RandomsTextureSmall),
                 nvrhi::BindingSetItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_0,
                                                    m_DeinterleavedDepthTextureArray.value),
                 nvrhi::BindingSetItem::Texture_UAV(VXGI_CT_COMMON_UAV_SLOT_0,
                                                    m_ScreenSpaceOcclusionTextureArray.value)});
            state.bindings = {bindingCache->GetOrCreateBindingSet(bindingSetDesc, m_ComputeBindingLayout)};
            commandList->setComputeState(state);

            auto &texDesc = m_DeinterleavedDepthTextureArray.value->getDesc();
            numGroups.x = div_ceil(texDesc.width, 8u);
            numGroups.y = div_ceil(texDesc.height, 8u);
            numGroups.z = texDesc.arraySize;
            commandList->dispatch(numGroups.x, numGroups.y, numGroups.z);
        }

        {
            state.pipeline = m_pBlurScreenSpaceOcclusionCS;
            bindingSetDesc.bindings.resize(numBindings);
            bindingSetDesc.bindings.insert(
                bindingSetDesc.bindings.end(),
                {nvrhi::BindingSetItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_0,
                                                    m_DeinterleavedDepthTextureArray.value),
                 nvrhi::BindingSetItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_1,
                                                    m_ScreenSpaceOcclusionTextureArray.value),
                 nvrhi::BindingSetItem::Texture_UAV(VXGI_CT_COMMON_UAV_SLOT_0,
                                                    m_ScreenSpaceOcclusionBlurTexture.value)});
            state.bindings = {bindingCache->GetOrCreateBindingSet(bindingSetDesc, m_ComputeBindingLayout)};
            commandList->setComputeState(state);

            auto &texDesc = m_ScreenSpaceOcclusionBlurTexture.value->getDesc();
            numGroups.x = div_ceil(texDesc.width, 16u);
            numGroups.y = div_ceil(texDesc.height, 16u);
            commandList->dispatch(numGroups.x, numGroups.y);
        }

        commandList->endMarker();
    }

    const bool renderDebug = m_PixelToSave.x >= 0 && m_PixelToSave.y >= 0;

    {
        commandList->beginMarker("Tracing: Diffuse");

        nvrhi::GraphicsState state;

        bindingSetDesc.bindings.resize(numBindings);
        bindingSetDesc.bindings.push_back(
            nvrhi::BindingSetItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_0, m_ConeDirectionsTexture));
        if (!m_AmbientOcclusionMode) {
            bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
                VXGI_CT_TEXTURE_SRV_SLOT_1,
                params.environmentMap ? params.environmentMap : m_NullCubemap.Get()));
        }
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
            VXGI_CT_TEXTURE_SRV_SLOT_2,
            enableSSAO ? m_ScreenSpaceOcclusionBlurTexture.value.Get() : m_NullTexture.Get()));

        nvrhi::IFramebuffer *fb;
        uint numTargets;
        if (m_AmbientOcclusionMode || sparsity <= 1u)
            numTargets = 1;
        else
            numTargets = 3;

        if (sparsity <= 1u) {
            fb = m_InterpolatedFbs[0];
        } else {
            if (useFiltering)
                fb = m_DownsampledSmoothedFb;
            else
                fb = m_DownsampledFb;
        }

        auto &pso = m_DiffuseTracingPSOs[(int)renderDebug][(int)(numTargets > 1)];
        if (!pso) {
            nvrhi::GraphicsPipelineDesc psoDesc;
            psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
            psoDesc.VS = m_Parent->GetFullScreenQuadVS();
            psoDesc.PS = renderDebug ? m_pDiffuseTracingDebugPS : m_pDiffuseTracingPS;
            psoDesc.bindingLayouts = {renderDebug ? m_GraphicsBindingLayoutDebug : m_GraphicsBindingLayout,
                                      VoxelTexture->GetTracingBindingLayout()};
            psoDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
            psoDesc.renderState.depthStencilState.depthTestEnable = false;
            psoDesc.renderState.depthStencilState.stencilEnable = false;

            nvrhi::FramebufferInfo fbInfo = fb->getFramebufferInfo();
            for (uint i = 0; i < numTargets; ++i) {
                psoDesc.renderState.blendState.targets[i]
                    .enableBlend()
                    .setBlendOp(nvrhi::BlendOp::Add)
                    .setSrcBlend(nvrhi::BlendFactor::One)
                    .setDestBlend(nvrhi::BlendFactor::One)
                    .setBlendOpAlpha(nvrhi::BlendOp::Add)
                    .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                    .setDestBlendAlpha(nvrhi::BlendFactor::One);
            }

            pso = m_Device->createGraphicsPipeline(psoDesc, fbInfo);
            NVRHI_ASSERT(pso);
            if (!pso)
                return Status::RESOURCE_CREATION_FAILED;
        }

        auto &fbDesc = fb->getDesc();
        for (uint n = 0; n < numTargets; ++n) {
            commandList->clearTextureFloat(fbDesc.colorAttachments[n].texture, nvrhi::AllSubresources,
                                           nvrhi::Color{0.f});
        }

        nvrhi::Viewport vp = inputBuffers->gbufferViewport;
        vp.minX = std::floor(vp.minX / (float)sparsity);
        vp.minY = std::floor(vp.minY / (float)sparsity);
        vp.maxX = std::ceil(vp.maxX / (float)sparsity);
        vp.maxY = std::ceil(vp.maxY / (float)sparsity);
        state.viewport.viewports = {vp};
        state.framebuffer = fb;
        state.pipeline = pso;

        if (renderDebug) {
            bindingSetDesc.bindings.insert(
                bindingSetDesc.bindings.end(),
                {nvrhi::BindingSetItem::StructuredBuffer_UAV(VXGI_CT_SAMPLE_POSITIONS_UAV_SLOT,
                                                             m_SamplePositions),
                 nvrhi::BindingSetItem::StructuredBuffer_UAV(VXGI_CT_CONE_DIRECTIONS_UAV_SLOT,
                                                             m_ConeDirections),
                 nvrhi::BindingSetItem::TypedBuffer_UAV(VXGI_CT_APPEND_COUNTERS_UAV_SLOT, m_AppendCounters)});
        }

        state.bindings = {
            bindingCache->GetOrCreateBindingSet(
                bindingSetDesc, renderDebug ? m_GraphicsBindingLayoutDebug : m_GraphicsBindingLayout),
            TracingBindingSet};

        commandList->setGraphicsState(state);

        float fullscreenCB[] = {params.nearClipZ, params.farClipZ, params.farClipZ};
        commandList->setPushConstants(fullscreenCB, sizeof(fullscreenCB));

        nvrhi::DrawArguments args;
        args.instanceCount = params.numCones;
        args.vertexCount = 4;
        commandList->draw(args);

        commandList->endMarker();
    }

    if (sparsity > 1u) {
        if (useFiltering) {
            commandList->beginMarker("Tracing: Diffuse Filtering");

            nvrhi::ComputeState state;
            state.pipeline = m_pInterpolateCoarseCS;

            bindingSetDesc.bindings.resize(numBindings);
            uint numTargets = m_AmbientOcclusionMode ? 1 : 3;
            for (uint slot = 0; slot < numTargets; ++slot) {
                bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
                    VXGI_CT_TEXTURE_SRV_SLOT_0 + slot, m_DownsampledSmoothedTargets[slot].value));
                bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(
                    VXGI_CT_COMMON_UAV_SLOT_0 + slot, m_DownsampledTargets[slot].value));
            }

            state.bindings = {bindingCache->GetOrCreateBindingSet(bindingSetDesc, m_ComputeBindingLayout),
                              TracingBindingSet};
            commandList->setComputeState(state);

            int2 blockDim{8 * (int)sparsity};
            numGroups.x = div_ceil(viewportSize.x, blockDim.x);
            numGroups.y = div_ceil(viewportSize.y, blockDim.y);
            commandList->dispatch(numGroups.x, numGroups.y);

            commandList->endMarker();
        }

        {
            commandList->beginMarker("Tracing: Interpolation");

            nvrhi::ComputeState state;
            state.pipeline = m_pInterpolateIlluminationCS;

            bindingSetDesc.bindings.resize(numBindings);

            for (uint k = 0; k < m_DownsampledTargetCount; ++k)
                bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
                    VXGI_CT_TEXTURE_SRV_SLOT_0 + k, m_DownsampledTargets[k].value));

            if (inputBuffersPreviousFrame && params.enableTemporalReprojection) {
                bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
                    VXGI_CT_TEXTURE_SRV_SLOT_3, m_InterpolatedTargets[1].value));
            } else {
                bindingSetDesc.bindings.push_back(
                    nvrhi::BindingSetItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_3, m_NullTexture));
            }

            if (enableSSAO)
                bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
                    VXGI_CT_TEXTURE_SRV_SLOT_4, m_ScreenSpaceOcclusionBlurTexture.value));
            else
                bindingSetDesc.bindings.push_back(
                    nvrhi::BindingSetItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_4, m_NullTexture));

            bindingSetDesc.bindings.insert(
                bindingSetDesc.bindings.end(),
                {nvrhi::BindingSetItem::Texture_UAV(0, m_InterpolatedTargets[0].value),
                 nvrhi::BindingSetItem::Texture_UAV(1, m_RefinementControlTexture.value),
                 nvrhi::BindingSetItem::Texture_UAV(2, m_RefinementGridTexture.value)});
            state.bindings = {bindingCache->GetOrCreateBindingSet(bindingSetDesc, m_ComputeBindingLayout),
                              TracingBindingSet};

            if (params.enableSparseTracingRefinement) {
                commandList->clearTextureUInt(m_RefinementControlTexture.value, nvrhi::AllSubresources, 0);
                commandList->clearTextureUInt(m_RefinementGridTexture.value, nvrhi::AllSubresources, 0);
            }

            commandList->setComputeState(state);

            numGroups.x = div_ceil(viewportSize.x, 16);
            numGroups.y = div_ceil(viewportSize.y, 16);
            commandList->dispatch(numGroups.x, numGroups.y);

            commandList->endMarker();
        }

        if (params.enableSparseTracingRefinement) {
            commandList->beginMarker("Tracing: Refinement");

            builtinConstants.m_vDownsampleScale = float4(1.f);
            commandList->writeBuffer(m_pTracingCB, &builtinConstants, sizeof(builtinConstants));

            commandList->clearDepthStencilTexture(m_RefinementStencilTexture.value, nvrhi::AllSubresources,
                                                  false, 0.f, true, 0);

            nvrhi::GraphicsState state;
            state.pipeline = m_pCopyToStencilPS;

            bindingSetDesc.bindings.resize(numBindings);
            bindingSetDesc.bindings.insert(bindingSetDesc.bindings.end(),
                                           {nvrhi::BindingSetItem::Texture_SRV(
                                                VXGI_CT_TEXTURE_SRV_SLOT_0, m_RefinementControlTexture.value),
                                            nvrhi::BindingSetItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_3,
                                                                               m_RefinementGridTexture.value)});
            state.bindings = {bindingCache->GetOrCreateBindingSet(bindingSetDesc, m_GraphicsBindingLayout)};
            state.viewport.viewports = {
                nvrhi::Viewport{(float)textureSize.x, (float)textureSize.y}
            };
            state.viewport.scissorRects = {
                nvrhi::Rect{(int)inputBuffers->gbufferViewport.minX, (int)inputBuffers->gbufferViewport.maxX,
                            (int)inputBuffers->gbufferViewport.minY, (int)inputBuffers->gbufferViewport.maxX}
            };
            state.framebuffer = m_RefinementStencilFb;
            commandList->setGraphicsState(state);

            nvrhi::DrawArguments args;
            auto &texDesc = m_RefinementGridTexture.value->getDesc();
            args.vertexCount = texDesc.height * 6 * texDesc.width;
            commandList->draw(args);

            bindingSetDesc.bindings.resize(numBindings);
            bindingSetDesc.bindings.push_back(
                nvrhi::BindingSetItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_0, m_ConeDirectionsTexture));
            if (!m_AmbientOcclusionMode) {
                bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
                    VXGI_CT_TEXTURE_SRV_SLOT_1,
                    params.environmentMap ? params.environmentMap : m_NullCubemap.Get()));
            }
            bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
                VXGI_CT_TEXTURE_SRV_SLOT_2,
                enableSSAO ? m_ScreenSpaceOcclusionBlurTexture.value.Get() : m_NullTexture.Get()));
            if (renderDebug) {
                bindingSetDesc.bindings.insert(bindingSetDesc.bindings.end(),
                                               {nvrhi::BindingSetItem::StructuredBuffer_UAV(
                                                    VXGI_CT_SAMPLE_POSITIONS_UAV_SLOT, m_SamplePositions),
                                                nvrhi::BindingSetItem::StructuredBuffer_UAV(
                                                    VXGI_CT_CONE_DIRECTIONS_UAV_SLOT, m_ConeDirections),
                                                nvrhi::BindingSetItem::TypedBuffer_UAV(
                                                    VXGI_CT_APPEND_COUNTERS_UAV_SLOT, m_AppendCounters)});
            }

            bindingSetDesc.bindings.push_back(
                nvrhi::BindingSetItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_3, m_RefinementGridTexture.value));
            state.bindings = {bindingCache->GetOrCreateBindingSet(bindingSetDesc, renderDebug ? m_GraphicsBindingLayoutDebug : m_GraphicsBindingLayout),
                              TracingBindingSet};

            state.pipeline = m_pDiffuseTracingRefinePS;
            state.framebuffer = m_DiffuseTracingRefineFbs[0];
            commandList->setGraphicsState(state);

            args.instanceCount = params.numCones;
            commandList->drawIndexed(args);

            commandList->endMarker();
        }
    }

    *outDiffuse = m_InterpolatedTargets[0].value;
    (*outDiffuse)->AddRef();

    return rc;
}

Status ViewTracer::SortDiffuseRenderTargetsForTR(nvrhi::ITexture *previousDepthTexture) {
    if (m_InterpolatedTargets[0].attachment == previousDepthTexture) {
        std::swap(m_InterpolatedTargets[0], m_InterpolatedTargets[1]);
        std::swap(m_InterpolatedFbs[0], m_InterpolatedFbs[1]);
        std::swap(m_DiffuseTracingRefineFbs[0], m_DiffuseTracingRefineFbs[1]);
    }

    if (m_InterpolatedTargets[1].attachment == previousDepthTexture)
        return Status::OK;
    else
        return Status::INVALID_ARGUMENT;
}

Status ViewTracer::SortSpecularRenderTargetsForTR(nvrhi::ITexture *previousDepthTexture) {
    if (m_SpecularTargets[0].attachment == previousDepthTexture) {
        std::swap(m_SpecularTargets[0], m_SpecularTargets[1]);
        std::swap(m_SpecularTracingFbs[0], m_SpecularTracingFbs[1]);
    }

    if (m_SpecularTargets[1].attachment == previousDepthTexture)
        return Status::OK;
    else
        return Status::INVALID_ARGUMENT;
}

Status ViewTracer::computeSpecularChannel(nvrhi::ICommandList *commandList,
                                          const SpecularTracingParameters &params,
                                          nvrhi::ITexture **outSpecular,
                                          const ViewTracerInputBuffers *inputBuffers,
                                          const ViewTracerInputBuffers *inputBuffersPreviousFrame) {
    Status rc = Status::OK;

    auto geometry = m_Parent->GetClipmapGeometry();
    auto VoxelTexture = m_Parent->GetVoxelTexture();
    auto &descDepth = inputBuffers->gbufferDepth->getDesc();
    uint2 textureSize = {descDepth.width, descDepth.height};
    uint sampleCount = descDepth.sampleCount;

    if (inputBuffersPreviousFrame) {
        auto &descDepthPrev = inputBuffersPreviousFrame->gbufferDepth->getDesc();
        uint2 textureSizePrev = uint2{descDepthPrev.width, descDepthPrev.height};

        if (inputBuffersPreviousFrame->gbufferDepth == inputBuffers->gbufferDepth &&
                !params.enableReprojectionFromSameFrame ||
            any(textureSizePrev != textureSize) ||
            VXGI_FAILED(SortSpecularRenderTargetsForTR(inputBuffersPreviousFrame->gbufferDepth))) {
            inputBuffersPreviousFrame = nullptr;
        }
    }

    m_SpecularTargets[0].attachment = inputBuffers->gbufferDepth;
    if (m_Parent->GetVoxelizationParameters()->emittanceFormat == EmittanceFormat::NONE) {
        donut::log::error("computeSpecularChannel is unsupported in Ambient Occlusion mode");
        return Status::INVALID_CONFIGURATION;
    }

    if (VXGI_FAILED(rc = LoadTracingShaders(0, sampleCount > 1)))
        return rc;

    int2 viewportSize{(int)(inputBuffers->gbufferViewport.maxX - inputBuffers->gbufferViewport.minX),
                      (int)(inputBuffers->gbufferViewport.maxY - inputBuffers->gbufferViewport.minY)};
    const bool useTemporalFilter =
        inputBuffersPreviousFrame && params.filter == SpecularTracingParameters::FILTER_TEMPORAL;
    const bool useSimpleFilter = params.filter == SpecularTracingParameters::FILTER_SIMPLE;

    BuiltinTracingConstants builtinConstants = {};
    uint2 gbufferTextureSize = textureSize;
    FillTracingConstants(&builtinConstants, params, inputBuffers, inputBuffersPreviousFrame,
                         gbufferTextureSize);

    builtinConstants.m_fDepthdeltaSign = params.farClipZ > params.nearClipZ ? 1.f : -1.f;
    builtinConstants.m_fInitialOffsetBias = params.initialOffsetBias;
    builtinConstants.m_fInitialOffsetDistanceFactor = params.initialOffsetDistanceFactor;
    builtinConstants.m_fEmittanceScale = params.irradianceScale;
    builtinConstants.m_bEnableSpecularRandomOffsets = 1;

    float temporalReprojectionWeight;
    if (useTemporalFilter)
        temporalReprojectionWeight = params.temporalReprojectionWeight;
    else
        temporalReprojectionWeight = 0.f;

    builtinConstants.m_fTemporalReprojectionWeight = temporalReprojectionWeight;
    builtinConstants.m_fReprojectionDepthWeightScale =
        1.f / (float)(params.temporalReprojectionMaxDistanceInVoxels * geometry->m_VoxelSizes[0]);
    builtinConstants.m_fReprojectionNormalWeightExponent = params.temporalReprojectionNormalWeightExponent;
    builtinConstants.m_vRandomOffset = useTemporalFilter ? int2(rand(), rand()) : int2(0);
    builtinConstants.m_fTangentJitterScale = params.tangentJitterScale;

    if (params.environmentMap) {
        builtinConstants.m_fEnvironmentMapTint = float4(params.environmentMapTint, 0.f);
        auto &envMapDesc = params.environmentMap->getDesc();
        builtinConstants.m_fEnvironmentMapResolution = envMapDesc.width;
        builtinConstants.m_fMaxEnvironmentMapMipLevel = std::max<int>(0, envMapDesc.mipLevels - 1);
    } else {
        builtinConstants.m_fEnvironmentMapTint = float4(0.f);
        builtinConstants.m_fEnvironmentMapResolution = 0.f;
        builtinConstants.m_fMaxEnvironmentMapMipLevel = 0.f;
    }

    float3 cameraDelta = float3(builtinConstants.m_GBuffer.cameraPosition) -
                         float3(builtinConstants.m_PreviousGBufer.cameraPosition);
    float cameraDeltaLength = dm::length(cameraDelta);
    float temporalReprojectionScale =
        geometry->m_VoxelSizes[0] / (geometry->m_VoxelSizes[0] + cameraDeltaLength);
    builtinConstants.m_fTemporalReprojectionWeight =
        builtinConstants.m_fTemporalReprojectionWeight * temporalReprojectionScale;
    commandList->writeBuffer(m_pTracingCB, &builtinConstants, sizeof(builtinConstants));

    if (VXGI_FAILED(rc = ValidateSpecularTargets(textureSize.x, textureSize.y, useSimpleFilter)))
        return rc;

    auto TracingBindingSet = VoxelTexture->GetBindingSetForIrradianceMapTracing();
    auto bindingCache = VoxelTexture->GetBindingCache();

    nvrhi::BindingSetDesc bindingSetDesc;

    bindingSetDesc.bindings = {
        nvrhi::BindingSetItem::PushConstants(0, sizeof(float3)),
        nvrhi::BindingSetItem::ConstantBuffer(VXGI_CT_BUILTIN_CBV_SLOT, m_pTracingCB),
        nvrhi::BindingSetItem::Texture_SRV(VXGI_CT_DEPTH_BUFFER_SRV_SLOT, inputBuffers->gbufferDepth),
        nvrhi::BindingSetItem::Texture_SRV(VXGI_CT_NORMAL_BUFFER_SRV_SLOT, inputBuffers->gbufferNormal),
        nvrhi::BindingSetItem::Texture_SRV(
            VXGI_CT_PREV_DEPTH_BUFFER_SRV_SLOT,
            useTemporalFilter ? inputBuffersPreviousFrame->gbufferDepth : m_NullTexture.Get()),
        nvrhi::BindingSetItem::Texture_SRV(
            VXGI_CT_PREV_NORMAL_BUFFER_SRV_SLOT,
            useTemporalFilter ? inputBuffersPreviousFrame->gbufferNormal : m_NullTexture.Get()),
        nvrhi::BindingSetItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_0, m_RandomsTextureSmall),
        nvrhi::BindingSetItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_1,
                                           params.environmentMap ? params.environmentMap : m_NullCubemap.Get()),
        nvrhi::BindingSetItem::Sampler(VXGI_CT_POINT_SAMPLER_SLOT, m_PointSampler),
        nvrhi::BindingSetItem::Sampler(VXGI_CT_LINEAR_BORDER_SAMPLER_SLOT, m_Parent->GetLinearWrapSampler()),
        nvrhi::BindingSetItem::Sampler(VXGI_CT_ENV_MAP_SAMPLER_SLOT, m_EnvironmentMapSampler)};
    const uint numBindings = bindingSetDesc.bindings.size();

    {
        commandList->beginMarker("Tracing: Specular");

        bindingSetDesc.bindings.insert(
            bindingSetDesc.bindings.end(),
            {nvrhi::BindingSetItem::Texture_SRV(
                VXGI_CT_TEXTURE_SRV_SLOT_2,
                useTemporalFilter ? m_SpecularTargets[1].value.Get() : m_NullTexture.Get())});

        const bool renderDebug = m_PixelToSave.x >= 0 && m_PixelToSave.y >= 0;
        if (renderDebug) {
            bindingSetDesc.bindings.insert(
                bindingSetDesc.bindings.end(),
                {nvrhi::BindingSetItem::StructuredBuffer_UAV(VXGI_CT_SAMPLE_POSITIONS_UAV_SLOT,
                                                             m_SamplePositions),
                 nvrhi::BindingSetItem::StructuredBuffer_UAV(VXGI_CT_CONE_DIRECTIONS_UAV_SLOT,
                                                             m_ConeDirections),
                 nvrhi::BindingSetItem::TypedBuffer_UAV(VXGI_CT_APPEND_COUNTERS_UAV_SLOT, m_AppendCounters)});
        }

        nvrhi::GraphicsState state;
        state.pipeline = renderDebug ? m_SpecularTracingPSOs[1] : m_SpecularTracingPSOs[0];
        state.bindings = {
            bindingCache->GetOrCreateBindingSet(
                bindingSetDesc, renderDebug ? m_GraphicsBindingLayoutDebug : m_GraphicsBindingLayout),
            TracingBindingSet};
        state.viewport.addViewport(inputBuffers->gbufferViewport);
        state.framebuffer = m_SpecularTracingFbs[0];

        commandList->clearTextureFloat(m_SpecularTargets[0].value, nvrhi::AllSubresources, nvrhi::Color{0.f});
        commandList->setGraphicsState(state);

        float fullscreenCB[] = {params.nearClipZ, params.farClipZ, 0.f};
        commandList->setPushConstants(&fullscreenCB, sizeof(fullscreenCB));
        nvrhi::DrawArguments args;
        args.vertexCount = 4;
        commandList->draw(args);

        commandList->endMarker();
    }

    nvrhi::ITexture *specularOutput;

    if (useSimpleFilter) {
        commandList->beginMarker("Tracing: Filter Specular");

        bindingSetDesc.bindings.resize(numBindings);
        bindingSetDesc.bindings.insert(
            bindingSetDesc.bindings.end(),
            {nvrhi::BindingSetItem::Texture_SRV(VXGI_CT_TEXTURE_SRV_SLOT_0, m_SpecularTargets[0].value),
             nvrhi::BindingSetItem::Texture_UAV(VXGI_CT_COMMON_UAV_SLOT_0, m_FilteredSpecularTarget.value)});

        nvrhi::ComputeState state;
        state.pipeline = m_pFilterSpecularCS;
        state.bindings = {bindingCache->GetOrCreateBindingSet(bindingSetDesc, m_ComputeBindingLayout),
                          TracingBindingSet};
        commandList->setComputeState(state);

        uint3 numGroups;
        numGroups.x = dm::div_ceil(viewportSize.x, 16);
        numGroups.y = dm::div_ceil(viewportSize.y, 16);
        commandList->dispatch(numGroups.x, numGroups.y);

        commandList->endMarker();

        specularOutput = m_FilteredSpecularTarget.value;
    } else
        specularOutput = m_SpecularTargets[0].value;

    *outSpecular = specularOutput;
    specularOutput->AddRef();

    return rc;
}

Status ViewTracer::renderSamplesDebug(nvrhi::ICommandList *commandList, nvrhi::ITexture *destinationTexture,
                                      nvrhi::ITexture *destinationDepth, const TracedSamplesParameters &params,
                                      const ViewTracerInputBuffers *inputBuffers) {
    Status rc = Status::OK;

    auto geometry = m_Parent->GetClipmapGeometry();
    auto VoxelizationParameters = m_Parent->GetVoxelizationParameters();
    auto &descDepth = inputBuffers->gbufferDepth->getDesc();
    uint2 textureSize{descDepth.width, descDepth.height};
    uint sampleCount = descDepth.sampleCount;

    const bool renderDebug = m_PixelToSave.x >= 0 && m_PixelToSave.y >= 0;

    if (renderDebug) {
        commandList->copyBuffer(m_DrawIndirectArgs, 0, m_AppendCounters, 0, 4);
        commandList->copyBuffer(m_DrawIndirectArgs, 16, m_AppendCounters, 4, 4);
        m_PixelToSave = {-1, -1, 0};
    }

    VisualizeSamplesConstants visualConstants;
    visualConstants.m_ViewProjMatrix = inputBuffers->viewMatrix * inputBuffers->projMatrix;
    visualConstants.m_SmallestVoxelSize = float4(geometry->m_VoxelSizes[0], 0.f);
    visualConstants.m_ClipmapCenter = float4(geometry->m_Center, 1.f / geometry->m_LevelSizes[0]);
    visualConstants.m_ClipmapCenterAtSave = m_ClipmapCenterAtSampleSave;
    visualConstants.m_TotalMipLevels = VoxelizationParameters->totalLevels;
    visualConstants.m_ColorSelection = params.colorMode;
    visualConstants.m_OnlyContributingSamples = params.onlyContributingSamples;
    visualConstants.m_ConeIndexFilter = params.coneIndexFilter;
    visualConstants.m_SampleIndexFilter = params.sampleIndexFilter;
    visualConstants.m_nStackSize = VoxelizationParameters->stackLevels;

    auto VoxelTexture = m_Parent->GetVoxelTexture();
    visualConstants.m_nStackTextureSize = VoxelTexture->GetLevelSize(0);
    visualConstants.m_ToroidalOffset =
        float4(float3(geometry->m_ClipmapToroidalOffsets[0]) / (float)geometry->m_LevelSizes[0], 0.f);
    visualConstants.m_nPackingStride = VoxelTexture->GetPackingStride();
    commandList->writeBuffer(m_VisualizeSamplesCB, &visualConstants, sizeof(visualConstants));

    auto bindingCache = VoxelTexture->GetBindingCache();
    auto TracingBindingLayout = VoxelTexture->GetTracingBindingLayout();
    auto TracingBindingSet = VoxelTexture->GetBindingSetForIrradianceMapTracing();

    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_VisualizeSamplesCB),
                               nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_SamplePositions),
                               nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_ConeDirections),
                               nvrhi::BindingSetItem::Texture_SRV(2, VoxelTexture->GetEmittanceTexture(0, 0))};

    {
        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(destinationTexture);
        fbDesc.depthAttachment = {destinationDepth};
        InvalidateFramebuffer(m_Device, fbDesc, m_DebugFramebuffer.GetAddressOf());
        NVRHI_ASSERT(m_DebugFramebuffer);
    }
    auto fbInfo = m_DebugFramebuffer->getFramebufferInfo();

    nvrhi::GraphicsState state;
    state.bindings = {bindingCache->GetOrCreateBindingSet(bindingSetDesc, m_pVisualizeBindingLayout),
                      TracingBindingSet};
    state.framebuffer = m_DebugFramebuffer;

    if (params.colorMode == TracedSamplesParameters::COLOR_TEXELS_LOWER_MIP ||
        params.colorMode == TracedSamplesParameters::COLOR_TEXELS_UPPER_MIP) {
        if (m_pVisualizeTexelsPSO) {
            if (m_pVisualizeTexelsPSO->getFramebufferInfo() != fbInfo)
                m_pVisualizeTexelsPSO = nullptr;
        }

        if (!m_pVisualizeTexelsPSO) {
            nvrhi::GraphicsPipelineDesc psoDesc;
            psoDesc.primType = nvrhi::PrimitiveType::PointList;
            psoDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
            psoDesc.VS = m_pVisualizeSamplesVS;
            psoDesc.GS = m_pVisualizeTexelsGS;
            psoDesc.PS = m_pVisualizeTexelsPS;
            psoDesc.bindingLayouts = {m_pVisualizeBindingLayout, TracingBindingLayout};

            m_pVisualizeTexelsPSO = m_Device->createGraphicsPipeline(psoDesc, fbInfo);
        }

        state.pipeline = m_pVisualizeTexelsPSO;

    } else {
        if (m_pVisualizeSamplesPSO) {
            if (m_pVisualizeSamplesPSO->getFramebufferInfo() != fbInfo)
                m_pVisualizeSamplesPSO = nullptr;
        }

        if (!m_pVisualizeSamplesPSO) {
            nvrhi::GraphicsPipelineDesc psoDesc;
            psoDesc.primType = nvrhi::PrimitiveType::PointList;
            psoDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
            psoDesc.VS = m_pVisualizeSamplesVS;
            psoDesc.GS = m_pVisualizeSamplesGS;
            psoDesc.PS = m_pVisualizeSamplesPS;
            psoDesc.bindingLayouts = {m_pVisualizeBindingLayout, TracingBindingLayout};

            m_pVisualizeSamplesPSO = m_Device->createGraphicsPipeline(psoDesc, fbInfo);
        }

        state.pipeline = m_pVisualizeSamplesPSO;
    }

    state.indirectParams = m_DrawIndirectArgs;
    state.viewport.addViewport(inputBuffers->gbufferViewport);

    commandList->setGraphicsState(state);
    commandList->drawIndexedIndirect(0);

    if (params.showConeDirections) {
        if (m_pVisualizeConesPSO) {
            if (m_pVisualizeSamplesPSO->getFramebufferInfo() != fbInfo)
                m_pVisualizeSamplesPSO = nullptr;
        }

        if (!m_pVisualizeConesPSO) {
            nvrhi::GraphicsPipelineDesc psoDesc;
            psoDesc.primType = nvrhi::PrimitiveType::PointList;
            psoDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
            psoDesc.renderState.depthStencilState.depthWriteEnable = false;
            psoDesc.VS = m_pVisualizeSamplesVS;
            psoDesc.GS = m_pVisualizeConesGS;
            psoDesc.PS = m_pVisualizeConesPS;
            psoDesc.bindingLayouts = {m_pVisualizeBindingLayout, TracingBindingLayout};

            m_pVisualizeConesPSO = m_Device->createGraphicsPipeline(psoDesc, fbInfo);
        }

        state.pipeline = m_pVisualizeConesPSO;

        commandList->setGraphicsState(state);
        commandList->dispatchIndirect(16);
    }

    return rc;
}

Status ViewTracer::renderTracerVision(nvrhi::ICommandList *commandList, const TracerVisionParameters &params,
                                      nvrhi::ITexture *destinationTexture,
                                      const ViewTracerInputBuffers *inputBuffers) {
    Status rc = Status::OK;

    auto geometry = m_Parent->GetClipmapGeometry();
    auto VoxelizationParameters = m_Parent->GetVoxelizationParameters();
    auto VoxelTexture = m_Parent->GetVoxelTexture();
    auto &descDepth = inputBuffers->gbufferDepth->getDesc();
    uint2 textureSize{descDepth.width, descDepth.height};
    uint sampleCount = descDepth.sampleCount;

    if (VXGI_FAILED(rc = LoadTracingShaders(0, sampleCount > 1)))
        return rc;

    BuiltinTracingConstants builtinConstants = {};
    FillTracingConstants(&builtinConstants, params, inputBuffers, nullptr, textureSize);
    builtinConstants.m_fConeFactor = GetConeFactor(params.coneAngle);
    builtinConstants.m_fEmittanceScale = params.irradianceScale * std::pow(builtinConstants.m_fConeFactor, -2);
    commandList->writeBuffer(m_pTracingCB, &builtinConstants, sizeof(builtinConstants));

    commandList->beginMarker("Tracer Vision");

    auto TracingBindingLayout = VoxelTexture->GetTracingBindingLayout();
    auto bindingCache = VoxelTexture->GetBindingCache();
    nvrhi::GraphicsState state;

    {
        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(destinationTexture);
        InvalidateFramebuffer(m_Device, fbDesc, m_TracerVisionFramebuffer.GetAddressOf());
        NVRHI_ASSERT(m_TracerVisionFramebuffer);
    }

    state.framebuffer = m_TracerVisionFramebuffer;

    const bool renderDebug = m_PixelToSave.x >= 0 && m_PixelToSave.y >= 0;
    auto fbInfo = m_TracerVisionFramebuffer->getFramebufferInfo();
    if (renderDebug) {
        if (m_TracerVisionPSOs[1]) {
            if (fbInfo != m_TracerVisionPSOs[1]->getFramebufferInfo())
                m_TracerVisionPSOs[1] = nullptr;
        }

        if (!m_TracerVisionPSOs[1]) {
            nvrhi::GraphicsPipelineDesc psoDesc;
            psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
            psoDesc.VS = m_Parent->GetFullScreenQuadVS();
            psoDesc.PS = m_pTracerVisionDebugPS;
            psoDesc.bindingLayouts = {m_GraphicsBindingLayoutDebug, TracingBindingLayout};
            m_TracerVisionPSOs[1] = m_Device->createGraphicsPipeline(psoDesc, fbInfo);
            NVRHI_ASSERT(m_TracerVisionPSOs[1]);
        }
        state.pipeline = m_TracerVisionPSOs[1];
    } else {
        if (m_TracerVisionPSOs[0]) {
            if (fbInfo != m_TracerVisionPSOs[0]->getFramebufferInfo())
                m_TracerVisionPSOs[0] = nullptr;
        }

        if (!m_TracerVisionPSOs[0]) {
            nvrhi::GraphicsPipelineDesc psoDesc;
            psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
            psoDesc.VS = m_Parent->GetFullScreenQuadVS();
            psoDesc.PS = m_pTracerVisionPS;
            psoDesc.bindingLayouts = {m_GraphicsBindingLayout, TracingBindingLayout};
            m_TracerVisionPSOs[0] = m_Device->createGraphicsPipeline(psoDesc, fbInfo);
            NVRHI_ASSERT(m_TracerVisionPSOs[0]);
        }
        state.pipeline = m_TracerVisionPSOs[0];
    }

    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(3, m_pTracingCB),
                               nvrhi::BindingSetItem::Texture_SRV(0, inputBuffers->gbufferDepth),
                               nvrhi::BindingSetItem::Texture_SRV(1, inputBuffers->gbufferNormal),
                               nvrhi::BindingSetItem::Texture_SRV(8, m_RandomsTextureSmall),
                               nvrhi::BindingSetItem::Sampler(0, m_PointSampler),
                               nvrhi::BindingSetItem::Sampler(1, m_Parent->GetLinearWrapSampler()),
                               nvrhi::BindingSetItem::Sampler(2, m_EnvironmentMapSampler)};

    if (renderDebug) {
        bindingSetDesc.bindings.insert(bindingSetDesc.bindings.end(),
                                       {nvrhi::BindingSetItem::StructuredBuffer_UAV(3, m_SamplePositions),
                                        nvrhi::BindingSetItem::StructuredBuffer_UAV(4, m_ConeDirections),
                                        nvrhi::BindingSetItem::TypedBuffer_UAV(5, m_AppendCounters)});
    }

    state.bindings = {bindingCache->GetOrCreateBindingSet(bindingSetDesc, m_GraphicsBindingLayout),
                      VoxelTexture->GetBindingSetForIrradianceMapTracing()};
    state.viewport.addViewport(inputBuffers->gbufferViewport);

    commandList->setGraphicsState(state);
    float fullscreenCB[] = {params.nearClipZ, params.farClipZ, 0.f};
    commandList->setPushConstants(fullscreenCB, sizeof(fullscreenCB));

    nvrhi::DrawArguments args;
    args.vertexCount = 4;
    commandList->draw(args);

    commandList->endMarker();

    return rc;
}

Status ViewTracer::ReleaseResources() {
    Status rc = Status::OK;
    bool invalidated;

    m_pTracingCB = nullptr;
    m_DownsampleCB = nullptr;
    m_pSamplerLinearBorder = nullptr;
    m_EnvironmentMapSampler = nullptr;
    m_DiscontinuitySampler = nullptr;
    m_SamplePositions = nullptr;
    m_ConeDirections = nullptr;
    m_DrawIndirectArgs = nullptr;
    m_AppendCounters = nullptr;
    m_VisualizeSamplesCB = nullptr;
    m_RandomsTextureSmall = nullptr;
    m_RandomsTextureLarge = nullptr;
    m_ConeDirectionsTexture = nullptr;
    m_NullTexture = nullptr;
    m_NullDepthStencilTexture = nullptr;
    m_NullCubemap = nullptr;
    m_PointSampler = nullptr;
    m_pVisualizeSamplesVS = nullptr;
    m_pVisualizeSamplesGS = nullptr;
    m_pVisualizeSamplesPS = nullptr;
    m_pVisualizeSamplesPSO = nullptr;
    m_pVisualizeTexelsGS = nullptr;
    m_pVisualizeTexelsPS = nullptr;
    m_pVisualizeTexelsPSO = nullptr;
    m_pVisualizeConesGS = nullptr;
    m_pVisualizeConesPS = nullptr;
    m_pVisualizeTexelsPSO = nullptr;
    m_pVisualizeSamplesPSO = nullptr;
    m_pVisualizeConesPSO = nullptr;

    m_DebugFramebuffer = nullptr;

    ReleaseTracingShaders();

    if (VXGI_FAILED(rc = ValidateDownsampledTargets(0, 0, true)))
        return rc;
    if (VXGI_FAILED(rc = ValidateDownsampledTargets(0, 0, false)))
        return rc;
    if (VXGI_FAILED(rc = ValidateSpecularTargets(0, 0, true)))
        return rc;
    if (VXGI_FAILED(rc = ValidateSpecularTargets(0, 0, false)))
        return rc;
    if (VXGI_FAILED(rc = ValidateInterpolatedTargets(0, 0)))
        return rc;
    if (VXGI_FAILED(rc = ValidateTextureSize(&m_ScreenSpaceOcclusionTextureArray, 0, 0, 1,
                                             nvrhi::Format::UNKNOWN, &invalidated)))
        return rc;
    if (VXGI_FAILED(rc = ValidateTextureSize(&m_ScreenSpaceOcclusionBlurTexture, 0, 0, 1,
                                             nvrhi::Format::UNKNOWN, &invalidated)))
        return rc;
    if (VXGI_FAILED(rc = ValidateTextureSize(&m_DeinterleavedDepthTextureArray, 0, 0, 1, nvrhi::Format::UNKNOWN,
                                             &invalidated)))
        return rc;

    m_PreviousTracingShaderSparsity = 0;
    memset(&m_PreviousDiffuseTracingParams, 0, sizeof(m_PreviousDiffuseTracingParams));

    return rc;
}

donut::engine::ShaderMacro ViewTracer::GetEmittanceFormatMacroForSampling() {
    auto VoxelTexture = m_Parent->GetVoxelTexture();
    return m_AmbientOcclusionMode ? VoxelTexture->GetEmittanceFormatForAO()
                                  : VoxelTexture->GetEmittanceFormatMacroForSampling();
}

}  // namespace vxgi
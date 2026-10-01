#include "VoxelTexture.h"
#include "AllocationMap.h"
#include "VoxelRenderer.h"
#include "VoxelizationCoverageMasks.h"
#include <donut/core/log.h>
#include <donut/engine/BindingCache.h>
#include <donut/engine/ShaderFactory.h>

#include "shaders/Common/AbstractConeTracingConstants.hlsli"

namespace vxgi {

static const char formatNames[6][16] = {"NONE", "", "",
                                        "UNORM8", "FLOAT16", "FLOAT32"};

static const char formatNames_0[6][16] = {"NONE", "", "",
                                          "UNORM8", "UNORM8", "FLOAT32"};

static const char formatNames_1[6][16] = {
    "NONE", "", "", "UNORM8", "FLOAT16_NVAPI", "UNORM8"};

donut::engine::ShaderMacro VoxelTexture::GetEmittanceFormatMacro() {
    return {"EMITTANCE_FORMAT",
            formatNames[(int)GetVoxelizationParameters()->emittanceFormat]};
}

donut::engine::ShaderMacro VoxelTexture::GetEmittanceFormatForAO() { return {"EMITTANCE_FORMAT", "NONE"}; }

nvrhi::Format VoxelTexture::GetEmittanceUAVFormat() {
    return GetVoxelizationParameters()->emittanceFormat ==
                   EmittanceFormat::FLOAT16
               ? nvrhi::Format::RGBA16_FLOAT
               : nvrhi::Format::R32_UINT;
}

donut::engine::ShaderMacro VoxelTexture::GetEmittanceFormatMacroTypedLoad() {
    if (GetVoxelizationParameters()->emittanceFormat !=
            EmittanceFormat::FLOAT16 ||
        !(m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11 ||
          m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12))
        return GetEmittanceFormatMacro();
    else
        return {"EMITTANCE_FORMAT", "FLOAT16_NVAPI"};
}

donut::engine::ShaderMacro VoxelTexture::GetEmittanceFormatMacroForSampling() {
    return {"EMITTANCE_FORMAT",
            formatNames_0[(int)GetVoxelizationParameters()->emittanceFormat]};
}

donut::engine::ShaderMacro
VoxelTexture::GetEmittanceFormatMacroForVoxelization() {
    return {"EMITTANCE_FORMAT",
            formatNames_1[(int)GetVoxelizationParameters()->emittanceFormat]};
}

VoxelTexture::VoxelTexture(VoxelRenderer *renderer) {
    m_Parent = renderer;
    m_Device = m_Parent->GetDevice();
    m_ShaderFactory = m_Parent->GetShaderFactory();
    m_AllocationMap = m_Parent->GetAllocationMap();

    auto params = GetVoxelizationParameters();

    m_BackupTextureSize = uint3(params->mapSize / 2, params->mapSize / 2,
                                (params->stackLevels - 1) * params->mapSize / 2);

    uint opacityOffset = 1;
    uint emittanceOffsetEven = 1;
    uint emittanceOffsetOdd = 1;
    for (uint level = 0; level < params->totalLevels; ++level) {
        m_OpacityPackingOffsets.push_back(opacityOffset);
        const bool isOdd = level & 1;
        uint &emittanceOffset = isOdd ? emittanceOffsetOdd : emittanceOffsetEven;
        m_EmittancePackingOffsets.push_back(emittanceOffset);
        uint levelSize = GetLevelSize(level);

        opacityOffset += levelSize + 2;
        emittanceOffset += levelSize + 2;
    }

    m_OpacityTextureSize = {params->mapSize, params->mapSize, opacityOffset};

    uint PackingStride = GetPackingStride();
    m_EmittanceTextureSize = {
        6 * PackingStride, params->mapSize,
        std::max<uint>(emittanceOffsetEven, emittanceOffsetOdd)};

    m_BindingCache = donut::MakeMono<donut::engine::BindingCache>(m_Device);
}

VoxelTexture::~VoxelTexture() {}

nvrhi::IFramebuffer *VoxelTexture::GetProjectedCoverageFramebuffer() {
    return m_ProjectedCoverageFramebuffer;
}

uint VoxelTexture::GetEmittanceChannelCount() {
    return GetVoxelizationParameters()->emittanceFormat ==
                   EmittanceFormat::FLOAT32
               ? 3
               : 1;
}

float VoxelTexture::GetEmittanceStorageScale() {
    return GetVoxelizationParameters()->emittanceStorageScale;
}

nvrhi::ITexture *VoxelTexture::GetEmittanceTexture(int channel, bool isOdd) {
    if (isOdd)
        return m_TextureEmittanceOdd[channel];
    else
        return m_TextureEmittanceEven[channel];
}

uint3 VoxelTexture::GetEmittanceTextureSize() { return m_EmittanceTextureSize; }

OpacityDirections VoxelTexture::GetOpacityDirectionCount() {
    return GetVoxelizationParameters()->opacityDirectionCount;
}

nvrhi::ITexture *VoxelTexture::GetOpacityTexture(bool negative) {
    return negative ? m_TextureCoverage_Neg : m_TextureCoverage_Pos;
}

uint3 VoxelTexture::GetOpacityTextureSize() { return m_OpacityTextureSize; }

uint VoxelTexture::GetPackingStride() {
    return GetVoxelizationParameters()->mapSize + 2;
}

uint VoxelTexture::GetLevelSize(uint level) {
    auto params = GetVoxelizationParameters();
    if (level < params->stackLevels)
        return params->mapSize;
    if (level < params->totalLevels)
        return (params->mapSize >> (level - params->stackLevels + 1));

    assert(0);
    return 0;
}

Status VoxelTexture::AllocateResources(nvrhi::ICommandList *commandList) {
    Status rc;
    auto params = GetVoxelizationParameters();

    {
        // IrradianceMapCB/ListProcessingCB: register(b0)

        // t_PagesToProcess : register(t0)
        // t_BackupOpacityTexture: register(t1)
        // t_EmittanceSrcR: register(t2), t_EmittanceSrc
        // t_EmittanceSrcG: register(t3)
        // t_EmittanceSrcB: register(t4)

        // u_CoverageTexture: register(u0)
        // u_CoverageTextureNeg: register(u1)
        // u_Opacity_Pos: register(u2)
        // u_IrradianceMap: register(u3)
        // u_IrradianceNormalization: register(u4)
        // u_BackupOpacityTexture: register(u5)
        // u_BackupOpacityTextureNeg: register(u6)
        // u_EmittanceR: register(u7), u_Emittance
        // u_EmittanceG: register(u8)
        // u_EmittanceB: register(u9)

        // s_LinearWrapSampler: register(s0)
        {
            nvrhi::BindingLayoutDesc bindingLayoutDesc;
            bindingLayoutDesc.visibility = nvrhi::ShaderType::Compute;
            bindingLayoutDesc.registerSpaceIsDescriptorSet = true;
            bindingLayoutDesc.bindings = {
                nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
                nvrhi::BindingLayoutItem::TypedBuffer_SRV(0),
                nvrhi::BindingLayoutItem::Texture_SRV(1),
                nvrhi::BindingLayoutItem::Texture_SRV(2),
                nvrhi::BindingLayoutItem::Texture_SRV(3),
                nvrhi::BindingLayoutItem::Texture_SRV(4),
                nvrhi::BindingLayoutItem::Texture_UAV(0),
                nvrhi::BindingLayoutItem::Texture_UAV(1),
                nvrhi::BindingLayoutItem::Texture_UAV(2),
                nvrhi::BindingLayoutItem::Texture_UAV(3),
                nvrhi::BindingLayoutItem::TypedBuffer_UAV(4),
                nvrhi::BindingLayoutItem::Texture_UAV(5),
                nvrhi::BindingLayoutItem::Texture_UAV(6),
                nvrhi::BindingLayoutItem::Texture_UAV(7),
                nvrhi::BindingLayoutItem::Texture_UAV(8),
                nvrhi::BindingLayoutItem::Texture_UAV(9),
                nvrhi::BindingLayoutItem::Sampler(0)};

            m_BindingLayout = m_Device->createBindingLayout(bindingLayoutDesc);

            bindingLayoutDesc.bindings = {nvrhi::BindingLayoutItem::TypedBuffer_UAV(4)};
            bindingLayoutDesc.registerSpaceIsDescriptorSet = true;
            m_NormalizationIrradianceScaleBindingLayout = m_Device->createBindingLayout(bindingLayoutDesc);
        }

        {
            nvrhi::BindingLayoutDesc bindingLayoutDesc;
            bindingLayoutDesc.visibility = nvrhi::ShaderType::All;
            bindingLayoutDesc.registerSpaceIsDescriptorSet = true;
            bindingLayoutDesc.registerSpace = VXGI_ACT_RESOURCE_SPACE;
            bindingLayoutDesc.bindings = {
                nvrhi::BindingLayoutItem::VolatileConstantBuffer(
                    VXGI_CONE_TRACING_CB_SLOT),
                nvrhi::BindingLayoutItem::VolatileConstantBuffer(
                    VXGI_CONE_TRACING_TRANSLATION_CB_SLOT),
                nvrhi::BindingLayoutItem::Texture_SRV(VXGI_OPACITY_POS_SRV_SLOT),
                nvrhi::BindingLayoutItem::Texture_SRV(VXGI_OPACITY_NEG_SRV_SLOT),
                nvrhi::BindingLayoutItem::Texture_SRV(VXGI_EMITTANCE_EVEN_R_SRV_SLOT),
                nvrhi::BindingLayoutItem::Texture_SRV(VXGI_EMITTANCE_EVEN_G_SRV_SLOT),
                nvrhi::BindingLayoutItem::Texture_SRV(VXGI_EMITTANCE_EVEN_B_SRV_SLOT),
                nvrhi::BindingLayoutItem::Texture_SRV(VXGI_EMITTANCE_ODD_R_SRV_SLOT),
                nvrhi::BindingLayoutItem::Texture_SRV(VXGI_EMITTANCE_ODD_G_SRV_SLOT),
                nvrhi::BindingLayoutItem::Texture_SRV(VXGI_EMITTANCE_ODD_B_SRV_SLOT),
                nvrhi::BindingLayoutItem::Sampler(VXGI_VOXELTEX_SAMPLER_SLOT)};

            m_TracingBindingLayout = m_Device->createBindingLayout(bindingLayoutDesc);
        }

        {
            nvrhi::BindingLayoutDesc bindingLayoutDesc;
            bindingLayoutDesc.visibility =
                nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
            bindingLayoutDesc.bindings = {
                nvrhi::BindingLayoutItem::PushConstants(0, 3 * sizeof(float)),
                nvrhi::BindingLayoutItem::VolatileConstantBuffer(1),
                nvrhi::BindingLayoutItem::Texture_SRV(0),
                nvrhi::BindingLayoutItem::Texture_SRV(1),
                nvrhi::BindingLayoutItem::Texture_SRV(2),
                nvrhi::BindingLayoutItem::Texture_SRV(3),
                nvrhi::BindingLayoutItem::Texture_SRV(4)};
            m_DebugBindingLayout = m_Device->createBindingLayout(bindingLayoutDesc);
            DONUT_ASSERT(m_DebugBindingLayout);
        }

        nvrhi::SamplerDesc samplerDesc;
        samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
        samplerDesc.setAllFilters(true);
        m_LinearWrapSampler = m_Device->createSampler(samplerDesc);
    }

    nvrhi::ShaderHandle cs;
    nvrhi::ComputePipelineDesc csoDesc;
    csoDesc.bindingLayouts = {m_BindingLayout};

    cs = m_ShaderFactory->CreateShader(
        "app/VoxelTexture/WrapOpacityClipmapCS.hlsl", "main", nullptr,
        nvrhi::ShaderType::Compute);
    csoDesc.CS = cs;
    m_WrapOpacityClipmapCS = m_Device->createComputePipeline(csoDesc);

    cs = m_ShaderFactory->CreateShader(
        "app/VoxelTexture/WrapOpacityMipmapCS.hlsl", "main", nullptr,
        nvrhi::ShaderType::Compute);
    csoDesc.CS = cs;
    m_WrapOpacityMipmapCS = m_Device->createComputePipeline(csoDesc);

    cs = m_ShaderFactory->CreateShader(
        "app/VoxelTexture/GenerateOpacityMipmapCS.hlsl", "main", nullptr,
        nvrhi::ShaderType::Compute);
    csoDesc.CS = cs;
    m_GenerateOpacityMipmapCS = m_Device->createComputePipeline(csoDesc);

    if (params->emittanceFormat != EmittanceFormat::NONE) {
        std::vector<donut::engine::ShaderMacro> emittanceWrapMacros{
            GetEmittanceFormatMacroTypedLoad()};

        cs = m_ShaderFactory->CreateShader(
            "app/VoxelTexture/WrapEmittanceClipmapCS.hlsl", "main",
            &emittanceWrapMacros, nvrhi::ShaderType::Compute);
        csoDesc.CS = cs;
        m_WrapEmittanceClipmapCS = m_Device->createComputePipeline(csoDesc);

        cs = m_ShaderFactory->CreateShader(
            "app/VoxelTexture/WrapEmittanceMipmapCS.hlsl", "main",
            &emittanceWrapMacros, nvrhi::ShaderType::Compute);
        csoDesc.CS = cs;
        m_WrapEmittanceMipmapCS = m_Device->createComputePipeline(csoDesc);

        std::vector<donut::engine::ShaderMacro> emittanceMipmapMacros{
            GetEmittanceFormatMacro(),
            {"USE_TRIANGULAR_FILTER",
                params->useHighQualityEmittanceDownsampling ? "1" : "0"}
        };

        cs = m_ShaderFactory->CreateShader(
            "app/VoxelTexture/GenerateEmittanceMipmapCS.hlsl", "main",
            &emittanceMipmapMacros, nvrhi::ShaderType::Compute);
        csoDesc.CS = cs;
        m_GenerateEmittanceMipmapCS = m_Device->createComputePipeline(csoDesc);
    }

    // Debug Graphics Pipelines
    {
        m_OpacityRaycastPS = m_ShaderFactory->CreateShader(
            "app/VoxelTexture/DebugOpacityPS.hlsl", "main", nullptr,
            nvrhi::ShaderType::Pixel);

        m_CoverageRaycastPS = m_ShaderFactory->CreateShader(
            "app/VoxelTexture/DebugCoveragePS.hlsl", "main", nullptr,
            nvrhi::ShaderType::Pixel);

        if (params->emittanceFormat != EmittanceFormat::NONE) {
            std::vector<donut::engine::ShaderMacro> debugEmittanceMacros = {
                GetEmittanceFormatMacroForSampling()};

            m_EmittanceRaycastPS = m_ShaderFactory->CreateShader(
                "app/VoxelTexture/DebugEmittancePS.hlsl", "main",
                &debugEmittanceMacros, nvrhi::ShaderType::Pixel);

            m_IrradianceRaycastPS = m_ShaderFactory->CreateShader(
                "app/VoxelTexture/DebugIrradiancePS.hlsl", "main", nullptr,
                nvrhi::ShaderType::Pixel);
        }
    }

    {
        std::vector<donut::engine::ShaderMacro> ClearConvertOpacityPagesMacros{
            {"USE_6D_OPACITY",
             params->opacityDirectionCount == OpacityDirections::SIX_DIMENSIONAL
                 ? "1"
                 : "0"}
        };

        auto CreatePageProcessPipeline =
            [&csoDesc, this](const char *sourceFile,
                             const std::vector<donut::engine::ShaderMacro> *macros,
                             bool allPagedLevels,
                             std::vector<nvrhi::ComputePipelineHandle> &pipelines) {
                Status rc;
                std::vector<nvrhi::ShaderHandle> shaderPermutations;
                if ((rc = m_AllocationMap->CreatePageProcessingCS(
                         sourceFile, allPagedLevels,
                         macros ? macros->data() : nullptr,
                         macros ? (uint)macros->size() : 0u, shaderPermutations)) !=
                    Status::OK)
                    return rc;

                pipelines.resize(shaderPermutations.size());
                for (uint i = 0; i < (uint)shaderPermutations.size(); ++i) {
                    csoDesc.CS = shaderPermutations[i];
                    pipelines[i] = m_Device->createComputePipeline(csoDesc);
                    if (!pipelines[i])
                        return Status::RESOURCE_CREATION_FAILED;
                }

                return Status::OK;
            };

        if ((rc = CreatePageProcessPipeline(
                 "app/VoxelTexture/ClearOpacityPagesCS.hlsl",
                 &ClearConvertOpacityPagesMacros, true, m_ClearOpacityPagesCS)) !=
            Status::OK)
            return rc;

        if ((rc = CreatePageProcessPipeline(
                 "app/VoxelTexture/ConvertCoverageToOpacityCS.hlsl",
                 &ClearConvertOpacityPagesMacros, false,
                 m_ConvertCoverageToOpacityCS)) != Status::OK)
            return rc;

        if ((rc = CreatePageProcessPipeline(
                 "app/VoxelTexture/RestoreOpacityPagesCS.hlsl", nullptr, false,
                 m_RestoreOpacityPagesCS)) != Status::OK)
            return rc;

        if ((rc = CreatePageProcessPipeline(
                 "app/VoxelTexture/DownsampleOpacityPagesCS.hlsl", nullptr, true,
                 m_DownsampleOpacityPagesCS)) != Status::OK)
            return rc;

        if (params->emittanceFormat != EmittanceFormat::NONE) {
            std::vector<donut::engine::ShaderMacro> clearEmittanceMacros{
                GetEmittanceFormatMacro(),
                {"PERSISTENT_VOXEL_DATA", params->persistentVoxelData ? "1" : "0"}
            };

            if ((rc = CreatePageProcessPipeline(
                     "app/VoxelTexture/ClearEmittancePagesCS.hlsl",
                     &clearEmittanceMacros, true, m_ClearEmittancePagesCS)) !=
                Status::OK)
                return rc;

            std::vector<donut::engine::ShaderMacro> downsampleEmittanceMacros{
                GetEmittanceFormatMacroTypedLoad(),
                {"USE_TRIANGULAR_FILTER",
                    params->useHighQualityEmittanceDownsampling ? "1" : "0"}
            };

            if ((rc = CreatePageProcessPipeline(
                     "app/VoxelTexture/DownsampleEmittancePagesCS.hlsl",
                     &downsampleEmittanceMacros, true,
                     m_DownsampleEmittancePagesCS)) != Status::OK)
                return rc;

            if (params->emittanceFormat != EmittanceFormat::FLOAT16) {
                std::vector<donut::engine::ShaderMacro> convertEmissivePagesMacros{
                    GetEmittanceFormatMacro(),
                };

                if ((rc = CreatePageProcessPipeline(
                         "app/VoxelTexture/ConvertEmissivePagesCS.hlsl",
                         &convertEmissivePagesMacros, false,
                         m_ConvertEmissivePagesCS)) != Status::OK)
                    return rc;
            }
        }
    }

    auto CreateOneTexture = [this](nvrhi::Format format, const uint3 &size,
                                   bool dummy, const char *debugName,
                                   nvrhi::TextureHandle &texture) {
        nvrhi::TextureDesc texDesc;
        texDesc.dimension = nvrhi::TextureDimension::Texture3D;
        texDesc.isUAV = true;
        texDesc.keepInitialState = true;
        texDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        texDesc.isTypeless = true;

        if (dummy) {
            texDesc.width = 1;
            texDesc.height = 1;
            texDesc.depth = 1;
        } else {
            texDesc.width = size.x;
            texDesc.height = size.y;
            texDesc.depth = size.z;
        }
        texDesc.format = format;
        texDesc.debugName = debugName;

        texture = m_Device->createTexture(texDesc);
        if (!texture)
            return Status::RESOURCE_CREATION_FAILED;
        return Status::OK;
    };

    {
        if ((rc = CreateOneTexture(
                 nvrhi::Format::R10G10B10A2_UNORM, m_OpacityTextureSize, false,
                 "textureCoveragePos", m_TextureCoverage_Pos)) != Status::OK)
            return rc;

        if ((rc = CreateOneTexture(
                 nvrhi::Format::R10G10B10A2_UNORM, m_OpacityTextureSize,
                 params->opacityDirectionCount !=
                     OpacityDirections::SIX_DIMENSIONAL,
                 "textureCoverageNeg", m_TextureCoverage_Neg)) != Status::OK)
            return rc;

        if ((rc = CreateOneTexture(
                 nvrhi::Format::R10G10B10A2_UNORM, m_BackupTextureSize,
                 !params->useOpacityInterpolation || !params->persistentVoxelData,
                 "textureBackupOpacityPos", m_TextureBackupOpacity_Pos)) !=
            Status::OK)
            return rc;

        if ((rc = CreateOneTexture(
                 nvrhi::Format::R10G10B10A2_UNORM, m_BackupTextureSize,
                 !params->useOpacityInterpolation || !params->persistentVoxelData ||
                     params->opacityDirectionCount !=
                         OpacityDirections::SIX_DIMENSIONAL,
                 "textureBackupOpacityNeg", m_TextureBackupOpacity_Neg)) !=
            Status::OK)
            return rc;
    }

    if (params->enableMultiBounce) {
        nvrhi::TextureDesc desc;
        desc.dimension = nvrhi::TextureDimension::Texture3D;
        desc.format = nvrhi::Format::RGBA16_FLOAT;
        desc.isUAV = true;
        desc.isTypeless = true;
        desc.width = params->indirectIrradianceMapSize;
        desc.height = params->indirectIrradianceMapSize;
        desc.depth = 6 * params->indirectIrradianceMapSize;
        desc.debugName = "textureIrradiance";
        desc.keepInitialState = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        m_IrradianceTexture = m_Device->createTexture(desc);
        if (!m_IrradianceTexture)
            return Status::RESOURCE_CREATION_FAILED;

        nvrhi::BufferDesc sumDesc;
        sumDesc.format = nvrhi::Format::R32_UINT;
        sumDesc.byteSize = 0x2000;
        sumDesc.canHaveUAVs = true;
        sumDesc.canHaveTypedViews = true;
        sumDesc.keepInitialState = true;
        sumDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        m_IrradianceNormalizationBuffer = m_Device->createBuffer(sumDesc);
        if (!m_IrradianceNormalizationBuffer)
            return Status::RESOURCE_CREATION_FAILED;

        const char groupSizes[][4] = {"1", "2", "4", "8", "16"};
        std::vector<donut::engine::ShaderMacro> traceIrradianceMapMacros{
            {"GROUP_SIZE", groupSizes[params->allocationMapLodBias -
                                      params->indirectIrradianceMapLodBias]}
        };

        cs = m_ShaderFactory->CreateShader(
            "app/VoxelTexture/ClearIrradiancePagesCS.hlsl", "main",
            &traceIrradianceMapMacros, nvrhi::ShaderType::Compute);
        if (!cs)
            return Status::RESOURCE_CREATION_FAILED;
        csoDesc.CS = cs;
        m_ClearIrradianceMapCS = m_Device->createComputePipeline(csoDesc);
        if (!m_ClearIrradianceMapCS)
            return Status::RESOURCE_CREATION_FAILED;

        traceIrradianceMapMacros.push_back(GetEmittanceFormatMacroForSampling());
        cs = m_ShaderFactory->CreateShader(
            "app/VoxelTexture/TraceIrradianceMapCS.hlsl", "main",
            &traceIrradianceMapMacros, nvrhi::ShaderType::Compute);
        if (!cs)
            return Status::RESOURCE_CREATION_FAILED;
        csoDesc.CS = cs;
        csoDesc.bindingLayouts.push_back(m_TracingBindingLayout);
        m_TraceIrradianceMapCS = m_Device->createComputePipeline(csoDesc);
        if (!m_TraceIrradianceMapCS)
            return Status::RESOURCE_CREATION_FAILED;

        csoDesc.bindingLayouts.pop_back();

        cs = m_ShaderFactory->CreateShader(
            "app/VoxelTexture/NormalizeIrradianceScaleCS.hlsl", "main", nullptr,
            nvrhi::ShaderType::Compute);
        if (!cs)
            return Status::RESOURCE_CREATION_FAILED;
        csoDesc.bindingLayouts = { m_NormalizationIrradianceScaleBindingLayout };
        csoDesc.CS = cs;
        m_NormalizeIrradianceScaleCS = m_Device->createComputePipeline(csoDesc);
        if (!m_NormalizeIrradianceScaleCS)
            return Status::RESOURCE_CREATION_FAILED;

        nvrhi::BufferDesc bufDesc;
        bufDesc.byteSize = sizeof(TraceIrradianceBuffer);
        bufDesc.isConstantBuffer = true;
        bufDesc.isVolatile = true;
        bufDesc.maxVersions = m_Parent->GetNumFramesInFlight();
        bufDesc.debugName = "VoxelTexture:TraceIrradianceBuffer";
        m_TraceIrradianceBuffer = m_Device->createBuffer(bufDesc);
        if (!m_TraceIrradianceBuffer)
            return Status::RESOURCE_CREATION_FAILED;
    } else {
        nvrhi::TextureDesc desc;
        desc.dimension = nvrhi::TextureDimension::Texture3D;
        desc.format = nvrhi::Format::RGBA16_FLOAT;
        desc.isUAV = false;
        desc.isTypeless = true;
        desc.width = 1;
        desc.height = 1;
        desc.depth = 1;
        desc.debugName = "textureIrradiance";
        // Nothing ever writes this placeholder, but it is still bound as an SRV by the
        // cone tracing passes. Without a declared initial state nvrhi has no recorded
        // transition for it and reports "Unknown prior state of texture ...", which is
        // why disabling multi-bounce aborted on startup. Matches the multi-bounce branch.
        desc.keepInitialState = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        m_IrradianceTexture = m_Device->createTexture(desc);
    }

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    samplerDesc.setMipFilter(false);
    m_IrradianceSampler = m_Device->createSampler(samplerDesc);

    // Emittance textures
    {
        uint emittanceChannelCount = 0;
        nvrhi::Format emittanceTextureFormat = nvrhi::Format::UNKNOWN;

        switch (params->emittanceFormat) {
            case EmittanceFormat::UNORM8:
                emittanceTextureFormat = nvrhi::Format::SRGBA8_UNORM;
                emittanceChannelCount = 3;
                break;
            case EmittanceFormat::FLOAT16:
                emittanceTextureFormat = nvrhi::Format::RGBA16_FLOAT;
                emittanceChannelCount = 1;
                break;
            case EmittanceFormat::FLOAT32:
                emittanceTextureFormat = nvrhi::Format::R32_FLOAT;
                emittanceChannelCount = 3;
                break;
            default:
                assert(0);
                return Status::INVALID_ARGUMENT;
        }

        m_TextureEmittanceEven.resize(emittanceChannelCount);
        m_TextureEmittanceOdd.resize(emittanceChannelCount);

        for (uint channel = 0; channel < emittanceChannelCount; ++channel) {
            const bool isDummy = channel >= GetEmittanceChannelCount();

            if ((rc = CreateOneTexture(emittanceTextureFormat, m_EmittanceTextureSize,
                                       isDummy, "textureEmittanceEven",
                                       m_TextureEmittanceEven[channel])) !=
                Status::OK)
                return rc;

            if ((rc = CreateOneTexture(emittanceTextureFormat, m_EmittanceTextureSize,
                                       isDummy, "textureEmittanceOdd",
                                       m_TextureEmittanceOdd[channel])) != Status::OK)
                return rc;
        }
    }

    {
        nvrhi::BufferDesc bufDesc;
        bufDesc.byteSize = sizeof(AllocationMap::RaycastCB);
        bufDesc.isConstantBuffer = true;
        bufDesc.isVolatile = true;
        bufDesc.maxVersions = m_Parent->GetNumFramesInFlight();
        bufDesc.debugName = "VoxelTexture:Debug";
        m_DebugBuffer = m_Device->createBuffer(bufDesc);

        bufDesc.byteSize = sizeof(VoxelizationBuffer);
        bufDesc.isVolatile = false;
        bufDesc.maxVersions = 0;
        bufDesc.keepInitialState = true;                         // Required by vulkan
        bufDesc.initialState = nvrhi::ResourceStates::CopyDest;  // Required by vulkan
        bufDesc.debugName = "VoxelTexture:Voxelization";
        m_VoxelizationBuffer = m_Device->createBuffer(bufDesc);

        bufDesc.byteSize = sizeof(VoxelizationMaterialBuffer);
        bufDesc.isVolatile = false;
        bufDesc.maxVersions = 0;
        bufDesc.keepInitialState = true; // Required by vulkan
        bufDesc.initialState = nvrhi::ResourceStates::CopyDest; // Required by vulkan
        bufDesc.debugName = "VoxelTexture:Material";
        m_VoxelizationMaterialBuffer = m_Device->createBuffer(bufDesc);

        bufDesc.byteSize = sizeof(ListProcessingBuffer);
        bufDesc.isVolatile = true;
        bufDesc.maxVersions = m_Parent->GetNumFramesInFlight();
        bufDesc.debugName = "VoxelTexture:ListProcessing";
        m_ListProcessingBuffers.resize(params->totalLevels);
        for (uint level = 0; level < params->totalLevels; ++level) {
            m_ListProcessingBuffers[level] = m_Device->createBuffer(bufDesc);
        }

        if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11 ||
            m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN) {
            uint depthTargetSize = params->mapSize;
            if (params->emittanceFormat != EmittanceFormat::NONE) {
                depthTargetSize *= 16u;
            }
            nvrhi::TextureDesc projectedCoverageDesc;
            projectedCoverageDesc.dimension = nvrhi::TextureDimension::Texture2DMS;
            projectedCoverageDesc.width = depthTargetSize;
            projectedCoverageDesc.height = depthTargetSize;
            projectedCoverageDesc.mipLevels = 1;
            projectedCoverageDesc.sampleCount = 8;
            projectedCoverageDesc.sampleQuality = 0;
            projectedCoverageDesc.format = nvrhi::Format::D16;
            projectedCoverageDesc.isRenderTarget = true;
            projectedCoverageDesc.useClearValue = true;
            projectedCoverageDesc.clearValue = nvrhi::Color{1.f};
            projectedCoverageDesc.isTypeless = true;
            projectedCoverageDesc.isUAV = false;
            projectedCoverageDesc.debugName = "projectedCoverage";
            m_ProjectedCoverageTexture =
                m_Device->createTexture(projectedCoverageDesc);
            commandList->beginTrackingTextureState(m_ProjectedCoverageTexture, nvrhi::AllSubresources,
                                                   nvrhi::ResourceStates::Common);
            commandList->clearDepthStencilTexture(m_ProjectedCoverageTexture, nvrhi::AllSubresources, true, 1.f,
                                                  true, 0);
            commandList->setPermanentTextureState(m_ProjectedCoverageTexture,
                                                  nvrhi::ResourceStates::DepthWrite);

            nvrhi::FramebufferDesc fbDesc;
            fbDesc.depthAttachment = {m_ProjectedCoverageTexture};
            m_ProjectedCoverageFramebuffer = m_Device->createFramebuffer(fbDesc);
        } else {
            nvrhi::FramebufferDesc fbDesc;
            m_ProjectedCoverageFramebuffer = m_Device->createFramebuffer(fbDesc);
        }

        bufDesc.isConstantBuffer = false;
        bufDesc.isVolatile = false;
        bufDesc.maxVersions = 0;
        bufDesc.byteSize = sizeof(ScissorStats);
        bufDesc.structStride = sizeof(ScissorStats);
        bufDesc.canHaveUAVs = 1;
        bufDesc.debugName = "VoxelTexture:ScissorStats";
        m_ScissorStatsBuffer = m_Device->createBuffer(bufDesc);
    }

    {
        nvrhi::BufferDesc coverageMasksBufferDesc;
        coverageMasksBufferDesc.byteSize = sizeof(g_VoxelizationCoverageMasksDX);
        coverageMasksBufferDesc.structStride = sizeof(uint4);
        coverageMasksBufferDesc.initialState = nvrhi::ResourceStates::CopyDest;
        coverageMasksBufferDesc.debugName = "VoxelTexture:CoverageMasksBuffer";
        m_CoverageMasksBuffer = m_Device->createBuffer(coverageMasksBufferDesc);

        commandList->beginTrackingBufferState(m_CoverageMasksBuffer,
                                              nvrhi::ResourceStates::CopyDest);
        commandList->writeBuffer(m_CoverageMasksBuffer,
                                 g_VoxelizationCoverageMasksDX,
                                 sizeof(g_VoxelizationCoverageMasksDX));
        commandList->setPermanentBufferState(m_CoverageMasksBuffer,
                                    nvrhi::ResourceStates::ShaderResource);
    }

    return Status::OK;
}

void VoxelTexture::ProcessOpacity(nvrhi::ICommandList *commandList) {
    commandList->beginMarker("Opacity: Coverage conversion");
    ConvertCoverageToOpacity(commandList);
    commandList->endMarker();
    commandList->beginMarker("Opacity: Downsampling");
    DownsampleOpacityPages(commandList);
    commandList->endMarker();
    commandList->beginMarker("Opacity: Mipmap Generation");
    GenerateOpacityMipmaps(commandList);
    commandList->endMarker();
}

void VoxelTexture::ClearOpacityPages(nvrhi::ICommandList *commandList) {
    auto params = GetVoxelizationParameters();

    if (m_Parent->GetCurrentPerGPUData()->m_OpacityCleared) {
        nvrhi::ComputeState state;
        uint dispatchBufferOffset;

        state.bindings.resize(1);

        for (uint level = 0; level < params->pageLevels; ++level) {
            nvrhi::BindingSetDesc bindingDesc;
            bindingDesc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0,
                                                      m_ListProcessingBuffers[level]),
                nvrhi::BindingSetItem::TypedBuffer_SRV(
                    0, m_AllocationMap->m_OpacityPagesToClear.buffer),
                nvrhi::BindingSetItem::Texture_UAV(0, m_TextureCoverage_Pos,
                                                   nvrhi::Format::R32_UINT)};
            if (params->opacityDirectionCount == OpacityDirections::SIX_DIMENSIONAL)
                bindingDesc.bindings.emplace_back(nvrhi::BindingSetItem::Texture_UAV(
                    1, m_TextureCoverage_Neg, nvrhi::Format::R32_UINT));

            auto bindingSet =
                m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout);

            state.pipeline = m_ClearOpacityPagesCS[level];
            state.bindings[0] = bindingSet;
            state.indirectParams = m_AllocationMap->m_OpacityPagesToClear.buffer;
            commandList->setComputeState(state);

            dispatchBufferOffset =
                m_AllocationMap->m_OpacityPagesToClear.getDispatchArgumentsOffset(
                    level);
            commandList->dispatchIndirect(dispatchBufferOffset);
        }
    } else {
        commandList->clearTextureUInt(m_TextureCoverage_Pos, nvrhi::AllSubresources, 0);
        if (params->opacityDirectionCount == OpacityDirections::SIX_DIMENSIONAL)
            commandList->clearTextureUInt(m_TextureCoverage_Neg, nvrhi::AllSubresources, 0);
        m_Parent->GetCurrentPerGPUData()->m_OpacityCleared = true;
    }
}

void VoxelTexture::RestoreOpacityPages(nvrhi::ICommandList *commandList) {
    auto params = GetVoxelizationParameters();
    nvrhi::ComputeState state;
    uint dispatchBufferOffset;

    state.bindings.resize(1);

    if (params->useOpacityInterpolation && params->persistentVoxelData) {
        state.bindings.resize(1);

        for (uint level = 1; level < params->stackLevels; ++level) {
            nvrhi::BindingSetDesc bindingDesc;
            bindingDesc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0,
                                                      m_ListProcessingBuffers[level]),
                nvrhi::BindingSetItem::Texture_UAV(0, m_TextureCoverage_Pos,
                                                   nvrhi::Format::R32_UINT),
                nvrhi::BindingSetItem::Texture_SRV(1, m_TextureBackupOpacity_Pos)};

            state.pipeline = m_RestoreOpacityPagesCS[level];
            state.bindings[0] =
                m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout);
            state.indirectParams = m_AllocationMap->m_OpacityPagesToRestore.buffer;

            dispatchBufferOffset =
                m_AllocationMap->m_OpacityPagesToRestore.getDispatchArgumentsOffset(
                    level);

            commandList->setComputeState(state);
            commandList->dispatchIndirect(dispatchBufferOffset);

            if (params->opacityDirectionCount == OpacityDirections::SIX_DIMENSIONAL) {
                bindingDesc.bindings[1] = nvrhi::BindingSetItem::Texture_UAV(
                    0, m_TextureCoverage_Neg, nvrhi::Format::R32_UINT);
                bindingDesc.bindings[2] =
                    nvrhi::BindingSetItem::Texture_SRV(1, m_TextureBackupOpacity_Neg);

                state.bindings[0] =
                    m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout);

                commandList->setComputeState(state);
                commandList->dispatchIndirect(dispatchBufferOffset);
            }
        }
    }
}

void VoxelTexture::ClearEmittancePages(nvrhi::ICommandList *commandList) {
    auto params = GetVoxelizationParameters();

    if (params->emittanceFormat != EmittanceFormat::NONE) {
        if (m_Parent->GetCurrentPerGPUData()->m_EmittanceCleared) {
            nvrhi::ComputeState state;
            uint dispatchBufferOffset;

            state.bindings.resize(1);

            for (uint level = 0; level < params->pageLevels; ++level) {
                nvrhi::BindingSetDesc bindingDesc;
                bindingDesc.bindings = {
                    nvrhi::BindingSetItem::ConstantBuffer(
                        0, m_ListProcessingBuffers[level]),
                    nvrhi::BindingSetItem::TypedBuffer_SRV(
                        0, m_AllocationMap->m_EmittancePagesToClear.buffer),
                };

                if (params->persistentVoxelData)
                    bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(
                        2, m_TextureCoverage_Pos, nvrhi::Format::R32_UINT));

                for (uint channel = 0; channel < GetEmittanceChannelCount();
                     ++channel) {
                    bool isOdd = level & 1;
                    bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(
                        7 + channel, GetEmittanceTexture(channel, isOdd),
                        GetEmittanceUAVFormat()));
                }

                state.pipeline = m_ClearEmittancePagesCS[level];
                state.bindings[0] =
                    m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout);
                state.indirectParams = m_AllocationMap->m_EmittancePagesToClear.buffer;
                dispatchBufferOffset =
                    m_AllocationMap->m_EmittancePagesToClear.getDispatchArgumentsOffset(
                        level);

                commandList->setComputeState(state);
                commandList->dispatchIndirect(dispatchBufferOffset);
            }
        } else {
            if (params->emittanceFormat == EmittanceFormat::UNORM8) {
                commandList->clearTextureUInt(GetEmittanceTexture(0, 0),
                                              nvrhi::AllSubresources, 0);
                commandList->clearTextureUInt(GetEmittanceTexture(0, 1),
                                              nvrhi::AllSubresources, 0);
            } else if (params->emittanceFormat == EmittanceFormat::FLOAT16 ||
                       params->emittanceFormat == EmittanceFormat::FLOAT32) {
                for (uint channel = 0; channel < GetEmittanceChannelCount();
                     ++channel) {
                    commandList->clearTextureFloat(GetEmittanceTexture(channel, 0),
                                                   nvrhi::AllSubresources, 0);
                    commandList->clearTextureFloat(GetEmittanceTexture(channel, 1),
                                                   nvrhi::AllSubresources, 0);
                }
            }

            m_Parent->GetCurrentPerGPUData()->m_EmittanceCleared = true;
        }
    }
}

void VoxelTexture::ConvertEmissivePages(nvrhi::ICommandList *commandList) {
    auto params = GetVoxelizationParameters();

    if (params->emittanceFormat != EmittanceFormat::NONE &&
        params->emittanceFormat != EmittanceFormat::FLOAT16) {
        nvrhi::ComputeState state;
        uint dispatchBufferOffset;

        state.bindings.resize(1);

        for (uint level = 0; level < params->stackLevels; ++level) {
            nvrhi::BindingSetDesc bindingDesc;
            bindingDesc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0,
                                                      m_ListProcessingBuffers[level]),
                nvrhi::BindingSetItem::TypedBuffer_SRV(
                    0,
                    m_AllocationMap->m_PagesWithEmissiveMaterialsToVoxelize.buffer),
                nvrhi::BindingSetItem::Texture_UAV(2, m_TextureCoverage_Pos,
                                                   nvrhi::Format::R32_UINT)};

            bool isOdd = level & 1;
            for (uint channel = 0; channel < GetEmittanceChannelCount(); ++channel) {
                bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(
                    7 + channel, GetEmittanceTexture(channel, isOdd),
                    GetEmittanceUAVFormat()));
            }

            state.pipeline = m_ConvertEmissivePagesCS[level];
            state.bindings[0] =
                m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout);
            state.indirectParams =
                m_AllocationMap->m_PagesWithEmissiveMaterialsToVoxelize.buffer;

            dispatchBufferOffset =
                m_AllocationMap->m_PagesWithEmissiveMaterialsToVoxelize
                    .getDispatchArgumentsOffset(level);
            commandList->setComputeState(state);
            commandList->dispatchIndirect(dispatchBufferOffset);
        }
    }
}

void VoxelTexture::WrapOpacity(nvrhi::ICommandList *commandList) {
    auto params = GetVoxelizationParameters();

    nvrhi::ComputeState state;

    nvrhi::BindingSetDesc bindingDesc;
    bindingDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_ListProcessingBuffers[0]),
        nvrhi::BindingSetItem::Texture_UAV(0, m_TextureCoverage_Pos,
                                           nvrhi::Format::R32_UINT)};

    state.pipeline = m_WrapOpacityClipmapCS;
    state.bindings = {
        m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout)};

    commandList->setComputeState(state);

    uint3 numGroups;
    numGroups.x = numGroups.y = div_ceil(params->mapSize, 8u);
    numGroups.z = params->stackLevels;
    commandList->dispatch(numGroups.x, numGroups.y, numGroups.z);

    if (params->opacityDirectionCount == OpacityDirections::SIX_DIMENSIONAL) {
        bindingDesc.bindings[1] = nvrhi::BindingSetItem::Texture_UAV(
            0, m_TextureCoverage_Neg, nvrhi::Format::R32_UINT);
        state.bindings[0] =
            m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout);
        commandList->setComputeState(state);
        commandList->dispatch(numGroups.x, numGroups.y, numGroups.z);
    }

    state.pipeline = m_WrapOpacityMipmapCS;

    for (uint level = params->stackLevels; level < params->totalLevels; ++level) {
        uint levelSize = GetLevelSize(level);

        bindingDesc.bindings[0] = nvrhi::BindingSetItem::ConstantBuffer(
            0, m_ListProcessingBuffers[level]);
        bindingDesc.bindings[1] = nvrhi::BindingSetItem::Texture_UAV(
            0, m_TextureCoverage_Pos, nvrhi::Format::R32_UINT);

        state.bindings[0] =
            m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout);

        numGroups.x = numGroups.y = (levelSize + 9) / 8;
        numGroups.z = 2;

        commandList->setComputeState(state);
        commandList->dispatch(numGroups.x, numGroups.y, numGroups.z);

        if (params->opacityDirectionCount == OpacityDirections::SIX_DIMENSIONAL) {
            bindingDesc.bindings[1] = nvrhi::BindingSetItem::Texture_UAV(
                0, m_TextureCoverage_Neg, nvrhi::Format::R32_UINT);

            state.bindings[0] =
                m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout);

            commandList->setComputeState(state);
            commandList->dispatch(numGroups.x, numGroups.y, numGroups.z);
        }
    }
}

void VoxelTexture::GenerateOpacityMipmaps(nvrhi::ICommandList *commandList) {
    auto params = GetVoxelizationParameters();
    nvrhi::ComputeState state;
    state.pipeline = m_GenerateOpacityMipmapCS;
    state.bindings.resize(1);

    for (uint level = params->pageLevels; level < params->totalLevels; ++level) {
        nvrhi::BindingSetDesc bindingDesc;
        bindingDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0,
                                                  m_ListProcessingBuffers[level]),
            nvrhi::BindingSetItem::Texture_UAV(0, m_TextureCoverage_Pos,
                                               nvrhi::Format::R32_UINT)};

        state.bindings[0] =
            m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout);

        uint levelSize = GetLevelSize(level);
        uint3 numGroups{div_ceil(levelSize, 4u)};
        commandList->setComputeState(state);
        commandList->dispatch(numGroups.x, numGroups.y, numGroups.z);

        if (params->opacityDirectionCount == OpacityDirections::SIX_DIMENSIONAL) {
            bindingDesc.bindings[1] = nvrhi::BindingSetItem::Texture_UAV(
                0, m_TextureCoverage_Neg, nvrhi::Format::R32_UINT);

            state.bindings[0] =
                m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout);

            commandList->setComputeState(state);
            commandList->dispatch(numGroups.x, numGroups.y, numGroups.z);
        }
    }
}

void VoxelTexture::GenerateEmittanceMipmaps(nvrhi::ICommandList *commandList) {
    auto params = GetVoxelizationParameters();

    if (params->emittanceFormat != EmittanceFormat::NONE) {
        nvrhi::ComputeState state;
        state.bindings.resize(1);

        for (uint level = params->pageLevels; level < params->totalLevels;
             ++level) {
            nvrhi::BindingSetDesc bindingDesc;
            bindingDesc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0,
                                                      m_ListProcessingBuffers[level]),
                nvrhi::BindingSetItem::Texture_UAV(2, m_TextureCoverage_Pos,
                                                   nvrhi::Format::R32_UINT),
                nvrhi::BindingSetItem::Sampler(0, m_LinearWrapSampler)};

            const bool isOdd = level & 1;

            for (uint channel = 0; channel < GetEmittanceChannelCount(); ++channel) {
                bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
                    2 + channel, GetEmittanceTexture(channel, !isOdd)));
                bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(
                    7 + channel, GetEmittanceTexture(channel, isOdd),
                    GetEmittanceUAVFormat()));
            }

            state.pipeline = m_GenerateEmittanceMipmapCS;
            state.bindings[0] =
                m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout);
            commandList->setComputeState(state);

            uint levelSize = GetLevelSize(level);
            uint3 numGroups = uint3(div_ceil(levelSize, 4u));
            commandList->dispatch(numGroups.x, numGroups.y, numGroups.z);

            state.pipeline = m_WrapEmittanceMipmapCS;

            bindingDesc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0,
                                                      m_ListProcessingBuffers[level]),
            };

            for (uint channel = 0; channel < GetEmittanceChannelCount(); ++channel) {
                bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(
                    7 + channel, GetEmittanceTexture(channel, isOdd),
                    GetEmittanceUAVFormat()));
            }

            state.bindings[0] =
                m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout);
            commandList->setComputeState(state);

            numGroups.x = numGroups.y = (levelSize + 9u) / 8u;
            numGroups.z = 2;
            commandList->dispatch(numGroups.x, numGroups.y, numGroups.z);
        }
    }
}

void VoxelTexture::DownsampleOpacityPages(nvrhi::ICommandList *commandList) {
    auto params = GetVoxelizationParameters();
    nvrhi::BindingSetDesc bindingDesc;
    nvrhi::ComputeState state;
    uint dispatchBufferOffset;

    bindingDesc.bindings.resize(4);
    state.bindings.resize(1);

    for (uint level = 1; level < params->pageLevels; ++level) {
        bindingDesc.bindings[0] = nvrhi::BindingSetItem::ConstantBuffer(
            0, m_ListProcessingBuffers[level]);
        bindingDesc.bindings[1] = nvrhi::BindingSetItem::TypedBuffer_SRV(
            0, m_AllocationMap->m_OpacityPagesToDownsample.buffer);
        bindingDesc.bindings[2] = nvrhi::BindingSetItem::Texture_UAV(
            0, m_TextureCoverage_Pos, nvrhi::Format::R32_UINT);
        bindingDesc.bindings[3] =
            nvrhi::BindingSetItem::Texture_SRV(1, m_TextureBackupOpacity_Pos);

        state.pipeline = m_DownsampleOpacityPagesCS[level];
        state.bindings[0] =
            m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout);
        state.indirectParams = m_AllocationMap->m_OpacityPagesToDownsample.buffer;

        commandList->setComputeState(state);

        dispatchBufferOffset =
            m_AllocationMap->m_OpacityPagesToDownsample.getDispatchArgumentsOffset(
                level);
        commandList->dispatchIndirect(dispatchBufferOffset);

        if (params->opacityDirectionCount == OpacityDirections::SIX_DIMENSIONAL) {
            bindingDesc.bindings[2] = nvrhi::BindingSetItem::Texture_UAV(
                0, m_TextureCoverage_Neg, nvrhi::Format::R32_UINT);
            bindingDesc.bindings[3] =
                nvrhi::BindingSetItem::Texture_SRV(1, m_TextureBackupOpacity_Neg);

            state.bindings[0] =
                m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout);

            commandList->setComputeState(state);
            commandList->dispatchIndirect(dispatchBufferOffset);
        }
    }
}

void VoxelTexture::DownsampleEmittancePages(nvrhi::ICommandList *commandList) {
    auto params = GetVoxelizationParameters();

    if (params->emittanceFormat != EmittanceFormat::NONE) {
        nvrhi::ComputeState state;
        state.pipeline = m_WrapEmittanceClipmapCS;
        state.bindings.resize(1);

        nvrhi::BindingSetDesc bindingDesc;
        bindingDesc.bindings.push_back(
            nvrhi::BindingSetItem::ConstantBuffer(0, m_ListProcessingBuffers[0]));
        for (uint channel = 0; channel < GetEmittanceChannelCount(); ++channel) {
            bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(
                7 + channel, GetEmittanceTexture(channel, false),
                GetEmittanceUAVFormat()));
        }
        state.bindings[0] =
            m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout);

        uint3 numGroups;
        numGroups.x = (params->mapSize + 9) / 8;
        numGroups.y = (params->mapSize + 7) / 8;
        numGroups.z = 1;
        commandList->setComputeState(state);
        commandList->dispatch(numGroups.x, numGroups.y, numGroups.z);

        uint dispatchBufferOffset;

        for (uint level = 1; level < params->pageLevels; ++level) {
            state.pipeline = m_DownsampleEmittancePagesCS[level];

            bindingDesc.bindings.clear();
            bindingDesc.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(
                0, m_ListProcessingBuffers[level]));
            bindingDesc.bindings.push_back(nvrhi::BindingSetItem::TypedBuffer_SRV(
                0, m_AllocationMap->m_EmittancePagesToDownsample.buffer));
            bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(
                2, m_TextureCoverage_Pos, nvrhi::Format::R32_UINT));

            bool isOdd = level & 1;
            for (uint channel = 0; channel < GetEmittanceChannelCount(); ++channel) {
                bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
                    2 + channel, GetEmittanceTexture(channel, !isOdd)));
                bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(
                    7 + channel, GetEmittanceTexture(channel, isOdd),
                    GetEmittanceUAVFormat()));
            }
            bindingDesc.bindings.push_back(
                nvrhi::BindingSetItem::Sampler(0, m_LinearWrapSampler));
            state.bindings[0] =
                m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout);

            state.indirectParams =
                m_AllocationMap->m_EmittancePagesToDownsample.buffer;
            dispatchBufferOffset = m_AllocationMap->m_EmittancePagesToDownsample
                                       .getDispatchArgumentsOffset(level);
            commandList->setComputeState(state);
            commandList->dispatchIndirect(dispatchBufferOffset);

            if (level > params->stackLevels) {
                state.pipeline = m_WrapEmittanceMipmapCS;
                state.indirectParams = nullptr;

                uint levelSize = GetLevelSize(level);
                numGroups.x = numGroups.y = (levelSize + 9) / 8;

                commandList->setComputeState(state);
                commandList->dispatch(numGroups.x, numGroups.y, 2u);
            } else {
                state.pipeline = m_WrapEmittanceClipmapCS;
                state.indirectParams = nullptr;

                commandList->setComputeState(state);
                commandList->dispatch(numGroups.x, numGroups.y, 1u);
            }
        }
    }
}

void VoxelTexture::ConvertCoverageToOpacity(nvrhi::ICommandList *commandList) {
    auto params = GetVoxelizationParameters();
    nvrhi::ComputeState state;
    nvrhi::BindingSetDesc bindingDesc;
    uint dispatchBufferOffset;

    if (params->opacityDirectionCount == OpacityDirections::SIX_DIMENSIONAL) {
        bindingDesc.bindings.resize(6);
    } else
        bindingDesc.bindings.resize(4);

    state.bindings.resize(1);

    for (uint level = 0; level < params->stackLevels; ++level) {
        state.pipeline = m_ConvertCoverageToOpacityCS[level];

        bindingDesc.bindings[0] = nvrhi::BindingSetItem::ConstantBuffer(
            0, m_ListProcessingBuffers[level]);
        bindingDesc.bindings[1] = nvrhi::BindingSetItem::TypedBuffer_SRV(
            0, m_AllocationMap->m_OpacityPagesToVoxelize.buffer);
        bindingDesc.bindings[2] = nvrhi::BindingSetItem::Texture_UAV(
            0, m_TextureCoverage_Pos, nvrhi::Format::R32_UINT);
        bindingDesc.bindings[3] = nvrhi::BindingSetItem::Texture_UAV(
            5, m_TextureBackupOpacity_Pos, nvrhi::Format::R32_UINT);
        if (params->opacityDirectionCount == OpacityDirections::SIX_DIMENSIONAL) {
            bindingDesc.bindings[4] = nvrhi::BindingSetItem::Texture_UAV(
                1, m_TextureCoverage_Neg, nvrhi::Format::R32_UINT);
            bindingDesc.bindings[5] = nvrhi::BindingSetItem::Texture_UAV(
                6, m_TextureBackupOpacity_Neg, nvrhi::Format::R32_UINT);
        }

        state.bindings[0] =
            m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout);

        state.indirectParams = m_AllocationMap->m_OpacityPagesToVoxelize.buffer;
        dispatchBufferOffset =
            m_AllocationMap->m_OpacityPagesToVoxelize.getDispatchArgumentsOffset(
                level);

        commandList->setComputeState(state);
        commandList->dispatchIndirect(dispatchBufferOffset);
    }
}

void VoxelTexture::FillVoxelizationBuffer(nvrhi::ICommandList *commandList,
                                          const MaterialInfo *material,
                                          bool isEmittance,
                                          float4 gridCenterPreviousFrame) {
    auto params = GetVoxelizationParameters();
    auto geometry = m_Parent->GetClipmapGeometry();
    VoxelizationBuffer voxelizationBuffer = {};

    voxelizationBuffer.gridCenter =
        float4(geometry->m_Center, 1.f / geometry->m_WorldSize);
    voxelizationBuffer.gridCenterPrevious = gridCenterPreviousFrame;
    voxelizationBuffer.toroidalOffset =
        int4(geometry->m_ClipmapToroidalOffsets[0], 0);
    voxelizationBuffer.allocationMapSize = params->allocationMapSize;
    voxelizationBuffer.irradianceMapSize = params->indirectIrradianceMapSize;
    voxelizationBuffer.clipLevelSize = params->mapSize;
    voxelizationBuffer.packingStride = GetPackingStride();
    voxelizationBuffer.maxClipLevel = params->stackLevels - 1;
    voxelizationBuffer.emittanceStorageScale = GetEmittanceStorageScale();
    voxelizationBuffer.clipLevelMask = m_VoxelizationClipLevelMask;
    voxelizationBuffer.useCullFunction = isEmittance;
    voxelizationBuffer.useIrradianceMap = params->enableMultiBounce;
    voxelizationBuffer.use6DOpacity =
        params->opacityDirectionCount == OpacityDirections::SIX_DIMENSIONAL;
    voxelizationBuffer.useFP32Emittance =
        params->emittanceFormat == EmittanceFormat::FLOAT32;
    voxelizationBuffer.persistentVoxelData = params->persistentVoxelData;
    voxelizationBuffer.useInvalidateBitmap =
        params->persistentVoxelData && params->simplifiedInvalidate &&
        !isEmittance && material->enableTriangleCulling;

    float discardNLevelDown;
    if (isEmittance) {
        discardNLevelDown = params->useEmittanceInterpolation ? 2.f : 1.f;
    } else {
        discardNLevelDown = params->useOpacityInterpolation ? 0.f : 1.f;
    }

    int discardHalfSize =
        int(std::pow(0.25f, discardNLevelDown) * (float)params->mapSize);

    voxelizationBuffer.firstLevelToDiscard =
        discardNLevelDown == 0.f ? 10 : (int)std::ceil(discardNLevelDown);
    voxelizationBuffer.discardClipSpace =
        std::pow(0.5f, (float)params->stackLevels + discardNLevelDown);
    voxelizationBuffer.discardLower =
        float(int(params->mapSize / 2) - discardHalfSize);
    voxelizationBuffer.discardUpper =
        float(int(params->mapSize / 2) + discardHalfSize - 1);
    memcpy(&voxelizationBuffer.textureToAmapTranslation,
           &m_VoxelizationTextureToAmapTranslation,
           sizeof(m_VoxelizationTextureToAmapTranslation));
    memcpy(&voxelizationBuffer.scissorRegionsClipSpace,
           &m_VoxelizationScissorRegionsClipSpace,
           sizeof(m_VoxelizationScissorRegionsClipSpace));

    commandList->writeBuffer(m_VoxelizationBuffer, &voxelizationBuffer,
                             sizeof(voxelizationBuffer));

    VoxelizationMaterialBuffer materialBuffer = {};
    materialBuffer.omnidirectionalLight = material->omnidirectionalLight;
    materialBuffer.proportionalEmittance = material->proportionalEmittance;
    materialBuffer.twoSided = material->twoSided;
    materialBuffer.frontCCW = material->frontCounterClockwise;

    int depthSampleCount;
    if ((int)(material->voxelizationThickness * 3.f) >= 1)
        depthSampleCount = (int)(material->voxelizationThickness * 3.f);
    else
        depthSampleCount = 1;

    depthSampleCount = std::min<int>(depthSampleCount, 6);

    materialBuffer.depthSamples = depthSampleCount - 1;
    materialBuffer.noiseScale = material->opacityNoiseScale;
    materialBuffer.noiseBias = material->opacityNoiseBias;

    for (uint clipLevel = 0; clipLevel < MAX_STACK_LEVELS; ++clipLevel) {
        float resolutionFactor =
            (float)GetResolutionFactor(material->materialSamplingRate, clipLevel);
        float4 resolutionFactors{resolutionFactor, 1.f / resolutionFactor, 0.f,
                                 0.f};
        materialBuffer.resolutionFactors[clipLevel] = resolutionFactors;
    }

    commandList->writeBuffer(m_VoxelizationMaterialBuffer, &materialBuffer,
                             sizeof(materialBuffer));
}

void VoxelTexture::prepareVoxelizationRenderState(
    const box3 &scissor, bool isEmittance,
    MaterialSamplingRate materialSamplingRate, uint numMaxViewports,
    uint *numViewports, nvrhi::Viewport *viewports, nvrhi::Rect *scissorRects) {
    if (materialSamplingRate == m_VoxelizationMaterialSamplingRate)
        return;

    m_VoxelizationMaterialSamplingRate = materialSamplingRate;

    auto params = GetVoxelizationParameters();
    auto geometry = m_Parent->GetClipmapGeometry();
    uint maxLevels = std::min<uint>(MAX_STACK_LEVELS, params->stackLevels);
    assert(numMaxViewports >= maxLevels * 3);
    *numViewports = 0;
    if (numMaxViewports < maxLevels * 3)
        return;

    int3 levelSize = {(int)GetLevelSize(0)};

    ibox3 textureBounds{int3{0}, int3{int(params->mapSize - 1)}};
    ibox3 finerLevelBounds{int3{int(params->mapSize / 4)},
                           int3{int(3 * (params->mapSize / 4) - 1)}};

    m_VoxelizationClipLevelMask = 0;

    float rWorldSize = 1.f / geometry->m_WorldSize;
    uint vIndex = 0;

    for (uint level = 0; level < maxLevels; ++level) {
        int viewportScale = 1 << (params->stackLevels - level - 1);
        int b = 1 - viewportScale;

        int3 halfLevelSize = levelSize / 2;
        int3 viewportTopLeft = halfLevelSize * b;
        int3 viewportSize = levelSize * viewportScale;

        float scale = rWorldSize * (float)viewportScale;

        box3 viewScissorF{scissor.lower() - geometry->m_Center,
                          scissor.upper() - geometry->m_Center};

        viewScissorF.lower() =
            (viewScissorF.lower() * scale + 0.5f) * float(params->mapSize);
        viewScissorF.upper() =
            (viewScissorF.upper() * scale + 0.5f) * float(params->mapSize);

        ibox3 viewScissorI{int3{viewScissorF.lower() - 1.f},
                           int3{viewScissorF.upper() + 1.f}};

        ibox3 viewScissor;

        if (viewScissorI.intersects(textureBounds) &&
            (!level || !finerLevelBounds.contains(viewScissorI) || !isEmittance)) {
            viewScissor = viewScissorI & textureBounds;
            m_VoxelizationClipLevelMask |= 1 << (3 * level);
        }

        float3 twoVoxels = {geometry->m_VoxelSizes[level] * 2.f};

        box3 levelPlusTwoVoxels = geometry->m_LevelRegions[level];
        levelPlusTwoVoxels.lower() -= twoVoxels;
        levelPlusTwoVoxels.upper() += twoVoxels;

        box3 scissorRegion = scissor & levelPlusTwoVoxels;
        scissorRegion.lower() =
            (scissorRegion.lower() - geometry->m_Center) * (rWorldSize * 2.f);
        scissorRegion.upper() =
            (scissorRegion.upper() - geometry->m_Center) * (rWorldSize * 2.f);

        m_VoxelizationScissorRegionsClipSpace[level] = {
            float4(scissorRegion.lower(), 0.f), float4(scissorRegion.upper(), 0.f)};

        int resolutionFactor = GetResolutionFactor(materialSamplingRate, level);
        viewScissor.lower() = viewScissor.lower() * resolutionFactor;
        viewScissor.upper() = viewScissor.upper() * resolutionFactor;

        viewportTopLeft = viewportTopLeft * resolutionFactor;
        viewportSize = viewportSize * resolutionFactor;

        int targetSize = resolutionFactor * params->mapSize;

        scissorRects[vIndex].minX = viewScissor.lower().x;
        scissorRects[vIndex].minY = targetSize - viewScissor.upper().y - 1;
        scissorRects[vIndex].maxX = viewScissor.upper().x + 1;
        scissorRects[vIndex].maxY = targetSize - viewScissor.lower().y;

        viewports[vIndex].minX = (float)viewportTopLeft.x;
        viewports[vIndex].minY = (float)viewportTopLeft.y;
        viewports[vIndex].minZ = 0.f;
        viewports[vIndex].maxX = float(viewportTopLeft.x + viewportSize.x);
        viewports[vIndex].maxY = float(viewportTopLeft.y + viewportSize.y);
        viewports[vIndex].maxZ = 1.f;

        vIndex += 1;

        scissorRects[vIndex].minX = viewScissor.lower().z;
        scissorRects[vIndex].minY = targetSize - viewScissor.upper().x - 1;
        scissorRects[vIndex].maxX = viewScissor.upper().z + 1;
        scissorRects[vIndex].maxY = targetSize - viewScissor.lower().x;

        viewports[vIndex].minX = (float)viewportTopLeft.z;
        viewports[vIndex].minY = (float)viewportTopLeft.x;
        viewports[vIndex].maxX = float(viewportTopLeft.z + viewportSize.z);
        viewports[vIndex].maxY = float(viewportTopLeft.x + viewportSize.x);

        vIndex += 1;

        scissorRects[vIndex].minX = viewScissor.lower().y;
        scissorRects[vIndex].minY = targetSize - viewScissor.upper().z - 1;
        scissorRects[vIndex].maxX = viewScissor.upper().y + 1;
        scissorRects[vIndex].maxY = targetSize - viewScissor.lower().z;

        viewports[vIndex].minX = (float)viewportTopLeft.y;
        viewports[vIndex].minY = (float)viewportTopLeft.z;
        viewports[vIndex].maxX = float(viewportTopLeft.y + viewportSize.y);
        viewports[vIndex].maxY = float(viewportTopLeft.z + viewportSize.z);

        vIndex += 1;

        int levelSizeFactor = params->stackLevels - level - 1;
        int shiftAmount = levelSizeFactor + params->allocationMapLodBias;

        int3 halfLevelSizeInPages = {(int)params->allocationMapSize >>
                                     (levelSizeFactor + 1)};
        int3 halfAmapSize = {(int)(params->allocationMapSize >> 1)};

        m_VoxelizationTextureToAmapTranslation[level] =
            int4(halfAmapSize - halfLevelSizeInPages +
                     geometry->m_AllocationMapToroidalOffsetWrapped,
                 shiftAmount);
    }

    *numViewports = vIndex;
}

void VoxelTexture::InvalidateVoxelizationRenderState() {
    m_VoxelizationMaterialSamplingRate = (MaterialSamplingRate)-1;
}

void VoxelTexture::ClearStatistics(nvrhi::ICommandList *commandList) {
    if (m_EnableStatistics)
        commandList->clearBufferUInt(m_ScissorStatsBuffer, 0);
}

void VoxelTexture::FillListProcessingBuffers(nvrhi::ICommandList *commandList) {
    auto params = GetVoxelizationParameters();
    auto geometry = m_Parent->GetClipmapGeometry();

    ListProcessingBuffer CB = {};
    CB.textureSize = uint4(GetOpacityTextureSize(), 0);
    CB.allocationLodBias = params->allocationMapLodBias;
    CB.stackSize = params->stackLevels;
    CB.numPagedLevels = params->pageLevels;
    CB.coverageBitCountMultiplier = 0.11111111f;
    CB.packingStride = GetPackingStride();
    CB.rEmittanceTextureSize = float4(1.f / float3(m_EmittanceTextureSize), 1.f);
    CB.persistentVoxelData = params->persistentVoxelData;

    CB.toroidalOffsetPrevious = int4{0};
    CB.translationParamsPrevious = int4{0};

    for (uint level = 0; level < params->totalLevels; ++level) {
        CB.levelSizePrevious = CB.levelSizeCurrent;
        CB.translationParamsPrevious = CB.translationParamsCurrent;
        CB.toroidalOffsetPrevious = CB.toroidalOffsetCurrent;
        CB.opacityPackingOffsetPrevious = CB.opacityPackingOffsetCurrent;
        CB.emittancePackingOffsetPrevious = CB.emittancePackingOffsetCurrent;
        CB.levelToProcess = level;
        CB.levelSizeCurrent = GetLevelSize(level);

        CB.toroidalOffsetCurrent =
            int4(geometry->m_ClipmapToroidalOffsets[level], 0);
        CB.opacityPackingOffsetCurrent = GetLodPackingOffset(level, false);
        CB.emittancePackingOffsetCurrent = GetLodPackingOffset(level, true);

        CB.clipmapAnchorOffset = float4((geometry->m_Anchor - geometry->m_Center) /
                                            geometry->m_LevelSizes[level],
                                        0.f);

        CB.useEmittanceInterpolation =
            params->useEmittanceInterpolation && level < params->stackLevels;

        if (level < params->pageLevels) {
            CB.listParams =
                m_AllocationMap->m_OpacityPagesToDownsample.getListParams(level);

            int levelOriginPage = 0;
            if (level < params->stackLevels - 1)
                levelOriginPage =
                    (params->allocationMapSize -
                     (params->allocationMapSize >> (params->stackLevels - level - 1))) /
                    2;

            CB.translationParamsCurrent =
                int4(levelOriginPage, levelOriginPage, levelOriginPage,
                     params->pageLevels - level - 1);
        }

        commandList->writeBuffer(m_ListProcessingBuffers[level], &CB, sizeof(CB));
    }
}

nvrhi::IBindingLayout *VoxelTexture::GetTracingBindingLayout() {
    return m_TracingBindingLayout;
}

nvrhi::IBindingSet *VoxelTexture::GetBindingSetForIrradianceMapTracing() {
    if (!m_TracingBindingSet) {
        nvrhi::BindingSetDesc bindingDesc;
        bindingDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(VXGI_CONE_TRACING_CB_SLOT,
                                                  m_Parent->GetAbstractTracingCB()),
            nvrhi::BindingSetItem::ConstantBuffer(
                VXGI_CONE_TRACING_TRANSLATION_CB_SLOT,
                m_Parent->GetTracingClipmapLevelCB()),
            nvrhi::BindingSetItem::Texture_SRV(VXGI_OPACITY_POS_SRV_SLOT,
                                               GetOpacityTexture(0)),
            nvrhi::BindingSetItem::Texture_SRV(VXGI_OPACITY_NEG_SRV_SLOT,
                                               GetOpacityTexture(1))};

        for (uint channel = 0; channel < GetEmittanceChannelCount(); ++channel)
            bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
                VXGI_EMITTANCE_EVEN_R_SRV_SLOT + channel,
                GetEmittanceTexture(channel, 0)));
        for (uint channel = 0; channel < GetEmittanceChannelCount(); ++channel)
            bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
                VXGI_EMITTANCE_ODD_R_SRV_SLOT + channel,
                GetEmittanceTexture(channel, 1)));

        bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Sampler(
            VXGI_VOXELTEX_SAMPLER_SLOT, m_Parent->GetLinearWrapSampler()));

        m_TracingBindingSet =
            m_Device->createBindingSet(bindingDesc, m_TracingBindingLayout);
    }

    return m_TracingBindingSet;
}

Status VoxelTexture::getVoxelizationState(nvrhi::ICommandList *commandList,
                                          bool isEmittance,
                                          const MaterialInfo &materialInfo,
                                          const float4 &gridCenterPreviousFrame,
                                          nvrhi::IBindingLayout *bindingLayout,
                                          nvrhi::IBindingSet **bindingSet) {
    Status rc;
    auto params = GetVoxelizationParameters();
    auto geometry = m_Parent->GetClipmapGeometry();
    int3 levelSize = GetLevelSize(0);
    VoxelizationBuffer voxelizationBuffer = {};
    voxelizationBuffer.gridCenter =
        float4(geometry->m_Center, 1.f / geometry->m_WorldSize);
    voxelizationBuffer.gridCenterPrevious = gridCenterPreviousFrame;
    voxelizationBuffer.toroidalOffset =
        int4(geometry->m_ClipmapToroidalOffsets[0], 0);
    voxelizationBuffer.allocationMapSize = params->allocationMapSize;
    voxelizationBuffer.irradianceMapSize = params->indirectIrradianceMapSize;
    voxelizationBuffer.clipLevelSize = params->mapSize;
    voxelizationBuffer.packingStride = GetPackingStride();
    voxelizationBuffer.maxClipLevel = params->stackLevels - 1;
    voxelizationBuffer.emittanceStorageScale = GetEmittanceStorageScale();
    voxelizationBuffer.clipLevelMask = m_VoxelizationClipLevelMask;
    voxelizationBuffer.useCullFunction = isEmittance;
    voxelizationBuffer.useIrradianceMap = params->enableMultiBounce;
    voxelizationBuffer.use6DOpacity =
        (params->opacityDirectionCount == OpacityDirections::SIX_DIMENSIONAL);
    voxelizationBuffer.useFP32Emittance =
        (params->emittanceFormat == EmittanceFormat::FLOAT32);
    voxelizationBuffer.persistentVoxelData = params->persistentVoxelData;
    voxelizationBuffer.useInvalidateBitmap =
        (params->persistentVoxelData && params->simplifiedInvalidate &&
         !isEmittance && materialInfo.enableTriangleCulling);

    float discardNLevelDown;
    if (isEmittance) {
        if (params->useEmittanceInterpolation)
            discardNLevelDown = 2.f;
        else
            discardNLevelDown = 1.f;
    } else {
        if (params->useOpacityInterpolation)
            discardNLevelDown = 0.f;
        else
            discardNLevelDown = 1.f;
    }

    int discardHalfSize =
        (int)(std::pow(0.25f, discardNLevelDown) * float(params->mapSize));

    voxelizationBuffer.firstLevelToDiscard =
        discardNLevelDown == 0.f ? 10 : (int)std::ceil(discardNLevelDown);
    voxelizationBuffer.discardClipSpace =
        std::pow(0.5f, float(params->stackLevels + discardNLevelDown));
    voxelizationBuffer.discardLower =
        float(params->mapSize / 2 - discardHalfSize);
    voxelizationBuffer.discardUpper =
        float(params->mapSize / 2 + discardHalfSize - 1);

    memcpy(voxelizationBuffer.textureToAmapTranslation,
           m_VoxelizationTextureToAmapTranslation,
           sizeof(m_VoxelizationTextureToAmapTranslation));
    memcpy(voxelizationBuffer.scissorRegionsClipSpace,
           m_VoxelizationScissorRegionsClipSpace,
           sizeof(m_VoxelizationScissorRegionsClipSpace));

    nvrhi::BindingSetDesc bindingDesc;

    if (isEmittance) {
        bindingDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(
                m_Parent->GetUserShaderBindingSlot(
                    UserShaderBindingID::VOXELIZE_CB),
                m_VoxelizationBuffer),
            nvrhi::BindingSetItem::ConstantBuffer(
                m_Parent->GetUserShaderBindingSlot(
                    UserShaderBindingID::VOXELIZE_MATERIAL_CB),
                m_VoxelizationMaterialBuffer),
            nvrhi::BindingSetItem::Texture_SRV(
                m_Parent->GetUserShaderBindingSlot(
                    UserShaderBindingID::INVALIDATE_BITMAP_SRV),
                m_AllocationMap->GetInvalidateBitmap()),
            nvrhi::BindingSetItem::Texture_UAV(
                m_Parent->GetUserShaderBindingSlot(
                    UserShaderBindingID::ALLOCATION_MAP_UAV),
                m_AllocationMap->GetTexture()),
        };
        if (m_EnableStatistics) {
            bindingDesc.bindings.push_back(
                nvrhi::BindingSetItem::StructuredBuffer_UAV(
                    m_Parent->GetUserShaderBindingSlot(
                        UserShaderBindingID::SCISSOR_STATS_UAV),
                    m_ScissorStatsBuffer));
        }

        bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
            m_Parent->GetUserShaderBindingSlot(
                UserShaderBindingID::IRRADIANCE_MAP_SRV),
            m_IrradianceTexture));

        bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(
            m_Parent->GetUserShaderBindingSlot(
                UserShaderBindingID::EMITTANCE_EVEN_R_UAV),
            GetEmittanceTexture(0, 0), GetEmittanceUAVFormat()));
        bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(
            m_Parent->GetUserShaderBindingSlot(
                UserShaderBindingID::EMITTANCE_ODD_R_UAV),
            GetEmittanceTexture(0, 1), GetEmittanceUAVFormat()));
        if (params->emittanceFormat == EmittanceFormat::FLOAT16) {
            bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
                m_Parent->GetUserShaderBindingSlot(
                    UserShaderBindingID::COVERAGE_POS_UAV),
                m_TextureCoverage_Pos));
        } else {
            bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(
                m_Parent->GetUserShaderBindingSlot(
                    UserShaderBindingID::EMITTANCE_EVEN_G_UAV),
                GetEmittanceTexture(1, 0), GetEmittanceUAVFormat()));
            bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(
                m_Parent->GetUserShaderBindingSlot(
                    UserShaderBindingID::EMITTANCE_EVEN_B_UAV),
                GetEmittanceTexture(2, 0), GetEmittanceUAVFormat()));
            bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(
                m_Parent->GetUserShaderBindingSlot(
                    UserShaderBindingID::EMITTANCE_ODD_G_UAV),
                GetEmittanceTexture(1, 1), GetEmittanceUAVFormat()));
            bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(
                m_Parent->GetUserShaderBindingSlot(
                    UserShaderBindingID::EMITTANCE_ODD_B_UAV),
                GetEmittanceTexture(2, 1), GetEmittanceUAVFormat()));
        }

        bindingDesc.bindings.push_back(nvrhi::BindingSetItem::Sampler(
            m_Parent->GetUserShaderBindingSlot(
                UserShaderBindingID::IRRADIANCE_MAP_SAMPLER),
            m_IrradianceSampler));
    } else {
        bindingDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(
                                    m_Parent->GetUserShaderBindingSlot(
                                        UserShaderBindingID::VOXELIZE_CB),
                                    m_VoxelizationBuffer),
                                nvrhi::BindingSetItem::ConstantBuffer(
                                    m_Parent->GetUserShaderBindingSlot(
                                        UserShaderBindingID::VOXELIZE_MATERIAL_CB),
                                    m_VoxelizationMaterialBuffer),
                                nvrhi::BindingSetItem::Texture_SRV(
                                    m_Parent->GetUserShaderBindingSlot(
                                        UserShaderBindingID::INVALIDATE_BITMAP_SRV),
                                    m_AllocationMap->GetInvalidateBitmap()),
                                nvrhi::BindingSetItem::Texture_UAV(
                                    m_Parent->GetUserShaderBindingSlot(
                                        UserShaderBindingID::ALLOCATION_MAP_UAV),
                                    m_AllocationMap->GetTexture()),
                                nvrhi::BindingSetItem::StructuredBuffer_SRV(
                                    m_Parent->GetUserShaderBindingSlot(
                                        UserShaderBindingID::COVERAGE_MASKS_SRV),
                                    m_CoverageMasksBuffer),
                                nvrhi::BindingSetItem::Texture_UAV(
                                    m_Parent->GetUserShaderBindingSlot(
                                        UserShaderBindingID::COVERAGE_POS_UAV),
                                    m_TextureCoverage_Pos, nvrhi::Format::R32_UINT),
                                nvrhi::BindingSetItem::Texture_UAV(
                                    m_Parent->GetUserShaderBindingSlot(
                                        UserShaderBindingID::COVERAGE_NEG_UAV),
                                    m_TextureCoverage_Neg,
                                    nvrhi::Format::R32_UINT)};

        if (m_EnableStatistics) {
            bindingDesc.bindings.push_back(
                nvrhi::BindingSetItem::StructuredBuffer_UAV(
                    m_Parent->GetUserShaderBindingSlot(
                        UserShaderBindingID::SCISSOR_STATS_UAV),
                    m_ScissorStatsBuffer));
        }
    }

    *bindingSet =
        m_BindingCache->GetOrCreateBindingSet(bindingDesc, bindingLayout)
            .Detach();
    commandList->writeBuffer(m_VoxelizationBuffer, &voxelizationBuffer,
                             sizeof(voxelizationBuffer));
    updateVoxelizationMaterialParameters(commandList, materialInfo);

    return Status::OK;
}

void VoxelTexture::updateVoxelizationMaterialParameters(
    nvrhi::ICommandList *commandList, const MaterialInfo &material) {
    VoxelizationMaterialBuffer materialBuffer;
    materialBuffer.omnidirectionalLight = material.omnidirectionalLight;
    materialBuffer.proportionalEmittance = material.proportionalEmittance;
    materialBuffer.twoSided = material.twoSided;
    materialBuffer.frontCCW = material.frontCounterClockwise;

    int numCube;
    if (int(material.voxelizationThickness * 3.f) > 1) {
        numCube = int(material.voxelizationThickness * 3.f);
    } else
        numCube = 1;
    numCube = std::min(numCube, 6);

    materialBuffer.depthSamples = numCube - 1;
    materialBuffer.noiseScale = material.opacityNoiseScale;
    materialBuffer.noiseBias = material.opacityNoiseBias;

    for (uint clipLevel = 0; clipLevel < MAX_STACK_LEVELS; ++clipLevel) {
        float resolutionFactor =
            GetResolutionFactor(material.materialSamplingRate, clipLevel);
        materialBuffer.resolutionFactors[clipLevel] =
            float4(resolutionFactor, 1.f / resolutionFactor, 0.f, 0.f);
    }

    commandList->writeBuffer(m_VoxelizationMaterialBuffer, &materialBuffer,
                             sizeof(materialBuffer));
}

const DerivedVoxelizationParameters *VoxelTexture::GetVoxelizationParameters() {
    return m_Parent->GetVoxelizationParameters();
}

int VoxelTexture::GetResolutionFactor(MaterialSamplingRate materialSamplingRate,
                                      uint clipmapLevel) {
    int result;
    switch (materialSamplingRate) {
        case MaterialSamplingRate::FIXED_2X:
            result = 2;
            break;
        case MaterialSamplingRate::FIXED_3X:
            result = 3;
            break;
        case MaterialSamplingRate::FIXED_4X:
            result = 4;
            break;
        case MaterialSamplingRate::ADAPTIVE_DEFAULT:
            result = std::min<int>(16, 1 << clipmapLevel);
            break;
        case MaterialSamplingRate::ADAPTIVE_GE2:
            result = std::max<int>(2, 1 << clipmapLevel);
            result = std::min<int>(16, result);
            break;
        case MaterialSamplingRate::ADAPTIVE_GE4:
            result = std::max<int>(4, 1 << clipmapLevel);
            result = std::min<int>(16, result);
            break;
        default:
            result = 1;
            break;
    }

    return result;
}

int VoxelTexture::GetLodPackingOffset(uint level, bool isEmittance) {
    auto params = GetVoxelizationParameters();
    if (level >= params->totalLevels) {
        donut::log::fatal("Provided level %u exceed maximum level %u", level,
                          params->totalLevels);
    }

    if (isEmittance)
        return m_EmittancePackingOffsets[level];
    else
        return m_OpacityPackingOffsets[level];
}

donut::engine::BindingCache *VoxelTexture::GetBindingCache() {
    return m_BindingCache;
}

void VoxelTexture::TraceIrradianceMap(
    nvrhi::ICommandList *commandList,
    const IndirectIrradianceMapTracingParameters *params) {
    auto VoxelizationParameters = GetVoxelizationParameters();
    auto geometry = m_Parent->GetClipmapGeometry();

    if (VoxelizationParameters->enableMultiBounce &&
        VoxelizationParameters->emittanceFormat != EmittanceFormat::NONE) {
        uint stackLevels = VoxelizationParameters->stackLevels;
        uint level =
            stackLevels +
            std::max<int>(0, VoxelizationParameters->indirectIrradianceMapLodBias) -
            1;

        TraceIrradianceBuffer d;

        auto IrradiancePageList = m_AllocationMap->GetIrradiancePageList(0);
        d.listParams = IrradiancePageList->getListParams(0);
        d.clipmapOrigin = float4(geometry->m_WorldRegion.lower(), 0.f);

        float voxelSize =
            geometry
                ->m_VoxelSizes[VoxelizationParameters->indirectIrradianceMapLod];
        float voxelHalfSize = voxelSize * 0.5f / geometry->m_VoxelSizes[0];

        d.irradianceVoxelSize =
            float4(voxelSize, voxelSize, voxelSize, voxelHalfSize);

        d.toroidalOffset =
            int4(geometry->m_ClipmapToroidalOffsets[level],
                 std::max<int>(
                     0, -VoxelizationParameters->indirectIrradianceMapLodBias));

        int LevelSize = GetLevelSize(level);
        int LodPackingOffset = GetLodPackingOffset(level, 0);

        d.sourceLevelSize = int4(int3(LevelSize), LodPackingOffset);

        d.irradianceMapSize = VoxelizationParameters->indirectIrradianceMapSize;
        d.logPageSize = VoxelizationParameters->allocationMapLodBias -
                        VoxelizationParameters->indirectIrradianceMapLodBias;

        d.tracingMaxSamples = params->maxSamples;
        d.tracingStep = params->tracingStep;
        d.tracingOpacityCorrectionFactor = params->opacityCorrectionFactor;
        d.tracingIrradianceScale = params->irradianceScale;
        d.tracingFlipOpacityDirections = params->flipOpacityDirections;
        d.tracingConeFactor = GetConeFactor(params->coneAngle);
        d.useIrradianceNormalization = params->useAutoNormalization;

        if (params->irradianceClampValue <= 0.f ||
            params->irradianceClampValue >= 65504.f)
            d.irradianceClampValue = 65504.f;
        else
            d.irradianceClampValue = params->irradianceClampValue;

        commandList->writeBuffer(m_TraceIrradianceBuffer, &d, sizeof(d));

        auto &indirectIrradianceCleared =
            m_Parent->GetCurrentPerGPUData()->m_IndirectIrradianceCleared;
        if (indirectIrradianceCleared) {
            nvrhi::ComputeState state;
            state.pipeline = m_ClearIrradianceMapCS;
            auto prevIrradiancePageList = m_AllocationMap->GetIrradiancePageList(1);

            nvrhi::BindingSetDesc bindingDesc;
            bindingDesc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, m_TraceIrradianceBuffer),
                nvrhi::BindingSetItem::TypedBuffer_SRV(
                    0, prevIrradiancePageList->buffer),
                nvrhi::BindingSetItem::Texture_UAV(3, m_IrradianceTexture),
            };
            state.bindings = {
                m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout)};
            state.indirectParams = prevIrradiancePageList->buffer;

            commandList->setComputeState(state);

            uint dispatchBufferOffset =
                prevIrradiancePageList->getDispatchArgumentsOffset(0);
            commandList->dispatchIndirect(dispatchBufferOffset);

            if (params->irradianceScale != m_PreviousIrradianceScale)
                commandList->clearBufferUInt(m_IrradianceNormalizationBuffer, 0);
        } else {
            commandList->clearTextureFloat(m_IrradianceTexture,
                                           nvrhi::AllSubresources, nvrhi::Color{0.f});
            commandList->clearBufferUInt(m_IrradianceNormalizationBuffer, 0);

            indirectIrradianceCleared = true;
        }

        m_PreviousIrradianceScale = params->irradianceScale;

        nvrhi::ComputeState state;
        state.pipeline = m_TraceIrradianceMapCS;

        IrradiancePageList = m_AllocationMap->GetIrradiancePageList(0);

        nvrhi::BindingSetDesc bindingDesc;
        bindingDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_TraceIrradianceBuffer),
            nvrhi::BindingSetItem::TypedBuffer_SRV(0, IrradiancePageList->buffer),
            nvrhi::BindingSetItem::Texture_UAV(3, m_IrradianceTexture),
            nvrhi::BindingSetItem::TypedBuffer_UAV(
                4, m_IrradianceNormalizationBuffer)};

        state.bindings.resize(2);
        state.bindings[0] =
            m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_BindingLayout);
        state.bindings[1] = GetBindingSetForIrradianceMapTracing();
        state.indirectParams = IrradiancePageList->buffer;
        commandList->setComputeState(state);

        uint dispatchBufferOffset =
            IrradiancePageList->getDispatchArgumentsOffset(0);
        commandList->dispatchIndirect(dispatchBufferOffset);

        if (params->useAutoNormalization) {
            state = {};
            state.pipeline = m_NormalizeIrradianceScaleCS;

            bindingDesc.bindings = {nvrhi::BindingSetItem::TypedBuffer_UAV(4, m_IrradianceNormalizationBuffer)};

            state.bindings = {m_BindingCache->GetOrCreateBindingSet(bindingDesc, m_NormalizationIrradianceScaleBindingLayout)};

            commandList->setComputeState(state);
            commandList->dispatch(1, 1, 1);
        }
    }
}

void VoxelTexture::RenderDebugOpacity(nvrhi::ICommandList *commandList,
                                      nvrhi::GraphicsState &state,
                                      const float4x4 &viewProjMatrix,
                                      float nearClipZ, float farClipZ,
                                      const float3 &cameraPos, uint level,
                                      uint voxelToSkip, bool renderAllLevels,
                                      float targetOpacity) {
    auto params = GetVoxelizationParameters();
    auto geometry = m_Parent->GetClipmapGeometry();

    if (m_OpacityRaycastPSO) {
        if (m_OpacityRaycastPSO->getFramebufferInfo() !=
            state.framebuffer->getFramebufferInfo())
            m_OpacityRaycastPSO = nullptr;
    }

    if (!m_OpacityRaycastPSO) {
        nvrhi::GraphicsPipelineDesc psoDesc;
        psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        psoDesc.bindingLayouts = {m_DebugBindingLayout};
        psoDesc.VS = m_Parent->GetFullScreenQuadVS();
        psoDesc.PS = m_OpacityRaycastPS;
        psoDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        psoDesc.renderState.rasterState.depthClipEnable = false;
        psoDesc.renderState.blendState.targets[0]
            .setBlendEnable(true)
            .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
            .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha);

        m_OpacityRaycastPSO = m_Device->createGraphicsPipeline(
            psoDesc, state.framebuffer->getFramebufferInfo());
        DONUT_ASSERT(m_OpacityRaycastPSO);
    }

    if (level < params->totalLevels) {
        const auto &viewport = state.viewport.viewports[0];
        AllocationMap::RaycastCB debugBuffer = {};
        debugBuffer.cameraPos = float4(cameraPos);
        debugBuffer.gridBounds = float4(geometry->m_LevelRegions[level].lower(),
                                        geometry->m_LevelSizes[level]);
        debugBuffer.viewProjMatrix = viewProjMatrix;
        debugBuffer.viewProjMatrixInv = inverse(viewProjMatrix);
        debugBuffer.textureSize = GetLevelSize(0);
        debugBuffer.opacityPackingOffset = GetLodPackingOffset(level, false);
        debugBuffer.toroidalOffset =
            int4(geometry->m_ClipmapToroidalOffsetsDebug[level], 0);
        if (level >= params->stackLevels)
            debugBuffer.mipOffset = level + 1 - params->stackLevels;
        else
            debugBuffer.mipOffset = 0;
        debugBuffer.voxelsToSkip = voxelToSkip;
        debugBuffer.skipUpperLevels = renderAllLevels && level;
        debugBuffer.farClipZ = farClipZ;
        debugBuffer.viewportMinZ = viewport.minZ;
        debugBuffer.viewportMaxZ = viewport.maxZ;
        debugBuffer.targetOpacity = targetOpacity;
        debugBuffer.use6DOpacity =
            GetOpacityDirectionCount() == OpacityDirections::SIX_DIMENSIONAL;

        commandList->writeBuffer(m_DebugBuffer, &debugBuffer, sizeof(debugBuffer));

        state.pipeline = m_OpacityRaycastPSO;
        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::PushConstants(0, 3 * sizeof(float)),
            nvrhi::BindingSetItem::ConstantBuffer(1, m_DebugBuffer),
            nvrhi::BindingSetItem::Texture_SRV(0, m_TextureCoverage_Pos),
            nvrhi::BindingSetItem::Texture_SRV(1, m_TextureCoverage_Neg)};

        state.bindings = {m_BindingCache->GetOrCreateBindingSet(
            bindingSetDesc, m_DebugBindingLayout)};

        commandList->setGraphicsState(state);

        float fullscreenCB[] = {nearClipZ, farClipZ, nearClipZ};
        commandList->setPushConstants(fullscreenCB, sizeof(fullscreenCB));
        nvrhi::DrawArguments args;
        args.vertexCount = 4;
        commandList->draw(args);
    }
}

void VoxelTexture::RenderDebugEmittance(nvrhi::ICommandList *commandList,
                                        nvrhi::GraphicsState &state,
                                        const float4x4 &viewProjMatrix,
                                        float nearClipZ, float farClipZ,
                                        const float3 &cameraPos, uint level,
                                        uint voxelToSkip, bool renderAllLevels,
                                        float targetOpacity) {
    auto params = GetVoxelizationParameters();
    auto geometry = m_Parent->GetClipmapGeometry();

    if (m_EmittanceRaycastPSO) {
        if (m_EmittanceRaycastPSO->getFramebufferInfo() !=
            state.framebuffer->getFramebufferInfo())
            m_EmittanceRaycastPSO = nullptr;
    }

    if (!m_EmittanceRaycastPSO) {
        nvrhi::GraphicsPipelineDesc psoDesc;
        psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        psoDesc.bindingLayouts = {m_DebugBindingLayout};
        psoDesc.VS = m_Parent->GetFullScreenQuadVS();
        psoDesc.PS = m_EmittanceRaycastPS;
        psoDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        psoDesc.renderState.rasterState.depthClipEnable = false;
        psoDesc.renderState.blendState.targets[0]
            .setBlendEnable(true)
            .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
            .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha);

        m_EmittanceRaycastPSO = m_Device->createGraphicsPipeline(
            psoDesc, state.framebuffer->getFramebufferInfo());
        DONUT_ASSERT(m_EmittanceRaycastPSO);
    }

    if (level < params->totalLevels &&
        params->emittanceFormat != EmittanceFormat::NONE) {
        const auto &viewport = state.viewport.viewports[0];
        AllocationMap::RaycastCB debugBuffer = {};
        debugBuffer.cameraPos = float4(cameraPos);
        debugBuffer.gridBounds = float4(geometry->m_LevelRegions[level].lower(),
                                        geometry->m_LevelSizes[level]);
        debugBuffer.viewProjMatrix = viewProjMatrix;
        debugBuffer.viewProjMatrixInv = inverse(viewProjMatrix);
        if (level >= params->stackLevels)
            debugBuffer.mipOffset = level + 1 - params->stackLevels;
        else
            debugBuffer.mipOffset = 0;
        debugBuffer.textureSize = GetLevelSize(0);
        debugBuffer.opacityPackingOffset = GetLodPackingOffset(level, false);
        debugBuffer.emittancePackingOffset = GetLodPackingOffset(level, true);
        debugBuffer.toroidalOffset =
            int4(geometry->m_ClipmapToroidalOffsetsDebug[level], 0);
        debugBuffer.voxelsToSkip = voxelToSkip;
        debugBuffer.scale = 1.f / GetEmittanceStorageScale();
        debugBuffer.skipUpperLevels = renderAllLevels && level;
        debugBuffer.farClipZ = farClipZ;
        debugBuffer.viewportMinZ = viewport.minZ;
        debugBuffer.viewportMaxZ = viewport.maxZ;
        debugBuffer.targetOpacity = targetOpacity;

        commandList->writeBuffer(m_DebugBuffer, &debugBuffer, sizeof(debugBuffer));

        nvrhi::Format emittanceSRVFormat =
            params->emittanceFormat == EmittanceFormat::UNORM8
                ? nvrhi::Format::SRGBA8_UNORM
                : nvrhi::Format::UNKNOWN;

        state.pipeline = m_EmittanceRaycastPSO;
        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::PushConstants(0, 3 * sizeof(float)),
            nvrhi::BindingSetItem::ConstantBuffer(1, m_DebugBuffer),
            nvrhi::BindingSetItem::Texture_SRV(0, m_TextureCoverage_Pos)};
        const bool IsOdd = level & 1;
        for (uint channel = 0; channel < GetEmittanceChannelCount(); ++channel) {
            bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
                2 + channel, GetEmittanceTexture(channel, IsOdd),
                emittanceSRVFormat));
        }

        state.bindings = {m_BindingCache->GetOrCreateBindingSet(
            bindingSetDesc, m_DebugBindingLayout)};

        commandList->setGraphicsState(state);

        float fullscreenCB[] = {nearClipZ, farClipZ, nearClipZ};
        commandList->setPushConstants(fullscreenCB, sizeof(fullscreenCB));
        nvrhi::DrawArguments args;
        args.vertexCount = 4;
        commandList->draw(args);
    }
}

void VoxelTexture::RenderDebugIrradiance(
    nvrhi::ICommandList *commandList, nvrhi::GraphicsState &state,
    const float4x4 &viewProjMatrix, float nearClipZ, float farClipZ,
    const float3 &cameraPos, uint voxelToSkip, float targetOpacity) {
    if (m_IrradianceRaycastPSO) {
        if (m_IrradianceRaycastPSO->getFramebufferInfo() !=
            state.framebuffer->getFramebufferInfo())
            m_IrradianceRaycastPSO = nullptr;
    }

    if (!m_IrradianceRaycastPSO) {
        nvrhi::GraphicsPipelineDesc psoDesc;
        psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        psoDesc.bindingLayouts = {m_DebugBindingLayout};
        psoDesc.VS = m_Parent->GetFullScreenQuadVS();
        psoDesc.PS = m_IrradianceRaycastPS;
        psoDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        psoDesc.renderState.rasterState.depthClipEnable = false;
        psoDesc.renderState.blendState.targets[0]
            .setBlendEnable(true)
            .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
            .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha);

        m_IrradianceRaycastPSO = m_Device->createGraphicsPipeline(
            psoDesc, state.framebuffer->getFramebufferInfo());
        DONUT_ASSERT(m_IrradianceRaycastPSO);
    }

    auto params = GetVoxelizationParameters();
    auto geometry = m_Parent->GetClipmapGeometry();
    if (params->enableMultiBounce) {
        const auto &viewport = state.viewport.viewports[0];
        AllocationMap::RaycastCB debugBuffer = {};
        debugBuffer.cameraPos = float4(cameraPos);
        debugBuffer.gridBounds =
            float4(geometry->m_WorldRegion.lower(), geometry->m_WorldSize);
        debugBuffer.viewProjMatrix = viewProjMatrix;
        debugBuffer.viewProjMatrixInv = inverse(viewProjMatrix);
        debugBuffer.mipOffset = 0;
        debugBuffer.textureSize = params->indirectIrradianceMapSize;
        debugBuffer.toroidalOffset = int4(0);
        debugBuffer.voxelsToSkip = voxelToSkip;
        debugBuffer.scale = 1.f / GetEmittanceStorageScale();
        debugBuffer.skipUpperLevels = 0;
        debugBuffer.farClipZ = farClipZ;
        debugBuffer.viewportMinZ = viewport.minZ;
        debugBuffer.viewportMaxZ = viewport.maxZ;
        debugBuffer.targetOpacity = targetOpacity;
        commandList->writeBuffer(m_DebugBuffer, &debugBuffer, sizeof(debugBuffer));

        state.pipeline = m_IrradianceRaycastPSO;
        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::PushConstants(0, 3 * sizeof(float)),
            nvrhi::BindingSetItem::ConstantBuffer(1, m_DebugBuffer),
            nvrhi::BindingSetItem::Texture_SRV(0, m_IrradianceTexture)};
        state.bindings = {m_BindingCache->GetOrCreateBindingSet(
            bindingSetDesc, m_DebugBindingLayout)};

        commandList->setGraphicsState(state);

        float fullscreenCB[] = {nearClipZ, farClipZ, nearClipZ};
        commandList->setPushConstants(fullscreenCB, sizeof(fullscreenCB));
        nvrhi::DrawArguments args;
        args.vertexCount = 4;
        commandList->draw(args);
    }
}

}  // namespace vxgi

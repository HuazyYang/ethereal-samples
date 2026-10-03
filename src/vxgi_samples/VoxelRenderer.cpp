#include "VoxelRenderer.h"
#include "AllocationMap.h"
#include "ViewTracer.h"
#include "VoxelTexture.h"
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/BindingCache.h>
#include <donut/core/log.h>

#include "shaders/UserDefined/UserDefinedConstants.hlsli"

namespace vxgi {

namespace {
// The register class a binding occupies (t/u/b/s): a set binds a layout item when it has an item of the same class
// in the same slot, which is how nvrhi's validation matches them.
char RegisterClass(nvrhi::ResourceType type) {
    switch (type) {
        case nvrhi::ResourceType::Texture_SRV:
        case nvrhi::ResourceType::TypedBuffer_SRV:
        case nvrhi::ResourceType::StructuredBuffer_SRV:
        case nvrhi::ResourceType::RawBuffer_SRV:
        case nvrhi::ResourceType::RayTracingAccelStruct:
            return 't';
        case nvrhi::ResourceType::Texture_UAV:
        case nvrhi::ResourceType::TypedBuffer_UAV:
        case nvrhi::ResourceType::StructuredBuffer_UAV:
        case nvrhi::ResourceType::RawBuffer_UAV:
        case nvrhi::ResourceType::SamplerFeedbackTexture_UAV:
            return 'u';
        case nvrhi::ResourceType::ConstantBuffer:
        case nvrhi::ResourceType::VolatileConstantBuffer:
        case nvrhi::ResourceType::PushConstants:
            return 'b';
        case nvrhi::ResourceType::Sampler:
            return 's';
        default:
            return 0;
    }
}
}  // namespace

BindingSetFactory::BindingSetFactory(nvrhi::IDevice* device) : m_Device(device) {}

nvrhi::IRHIObject* BindingSetFactory::GetPlaceholder(nvrhi::ResourceType type, uint32_t slot) {
    const bool uav = RegisterClass(type) == 'u';
    const auto key = std::make_pair(type, uav ? slot : 0u);
    auto it = m_Placeholders.find(key);
    if (it != m_Placeholders.end())
        return it->second.Get();

    // keepInitialState: nvrhi moves each placeholder into its state on first use on every backend (a Vulkan
    // resource starts out undefined), so no command list has to initialize them up front.
    nvrhi::AutoPtr<nvrhi::IRHIObject> placeholder;
    switch (type) {
        case nvrhi::ResourceType::Texture_SRV:
        case nvrhi::ResourceType::Texture_UAV: {
            nvrhi::TextureDesc desc;
            desc.width = 1;
            desc.height = 1;
            desc.format = nvrhi::Format::R32_UINT;
            desc.isUAV = uav;
            desc.initialState = uav ? nvrhi::ResourceStates::UnorderedAccess : nvrhi::ResourceStates::ShaderResource;
            desc.keepInitialState = true;
            desc.debugName = uav ? "VXGI placeholder UAV" : "VXGI placeholder SRV";
            nvrhi::TextureHandle texture;
            m_Device->createTexture(desc, &texture);
            placeholder = texture.Get();
            break;
        }
        case nvrhi::ResourceType::TypedBuffer_SRV:
        case nvrhi::ResourceType::TypedBuffer_UAV:
        case nvrhi::ResourceType::StructuredBuffer_SRV:
        case nvrhi::ResourceType::StructuredBuffer_UAV:
        case nvrhi::ResourceType::RawBuffer_SRV:
        case nvrhi::ResourceType::RawBuffer_UAV:
        case nvrhi::ResourceType::ConstantBuffer: {
            nvrhi::BufferDesc desc;
            desc.byteSize = 256;
            desc.canHaveUAVs = uav;
            if (type == nvrhi::ResourceType::TypedBuffer_SRV || type == nvrhi::ResourceType::TypedBuffer_UAV) {
                desc.format = nvrhi::Format::R32_UINT;
                desc.canHaveTypedViews = true;
            } else if (type == nvrhi::ResourceType::StructuredBuffer_SRV ||
                       type == nvrhi::ResourceType::StructuredBuffer_UAV) {
                desc.structStride = 4;
            } else if (type == nvrhi::ResourceType::RawBuffer_SRV || type == nvrhi::ResourceType::RawBuffer_UAV) {
                desc.canHaveRawViews = true;
            } else {
                desc.isConstantBuffer = true;
            }
            desc.initialState = type == nvrhi::ResourceType::ConstantBuffer ? nvrhi::ResourceStates::ConstantBuffer
                                : uav ? nvrhi::ResourceStates::UnorderedAccess
                                      : nvrhi::ResourceStates::ShaderResource;
            desc.keepInitialState = true;
            desc.debugName = "VXGI placeholder buffer";
            nvrhi::BufferHandle buffer;
            m_Device->createBuffer(desc, &buffer);
            placeholder = buffer.Get();
            break;
        }
        case nvrhi::ResourceType::Sampler: {
            nvrhi::SamplerHandle sampler;
            m_Device->createSampler(nvrhi::SamplerDesc(), &sampler);
            placeholder = sampler.Get();
            break;
        }
        default:
            return nullptr;
    }

    m_Placeholders.emplace(key, placeholder);
    return placeholder.Get();
}

nvrhi::BindingSetDesc BindingSetFactory::Complete(const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout) {
    const nvrhi::BindingLayoutDesc* layoutDesc = layout ? layout->getDesc() : nullptr;
    if (!layoutDesc)
        return desc;

    nvrhi::BindingSetDesc result = desc;
    for (const nvrhi::BindingLayoutItem& item : layoutDesc->bindings) {
        const char regClass = RegisterClass(item.type);
        for (uint32_t element = 0; element < item.getArraySize(); ++element) {
            const uint32_t slot = item.slot + element;
            bool bound = false;
            for (const nvrhi::BindingSetItem& binding : desc.bindings) {
                if (RegisterClass(binding.type) == regClass && binding.slot + binding.arrayElement == slot) {
                    bound = true;
                    break;
                }
            }
            if (bound)
                continue;

            nvrhi::BindingSetItem placeholder;
            if (item.type == nvrhi::ResourceType::PushConstants) {
                placeholder = nvrhi::BindingSetItem::PushConstants(item.slot, item.size);
            } else {
                // VolatileConstantBuffer and anything unexpected: left unbound, so the omission is still reported.
                nvrhi::IRHIObject* resource = GetPlaceholder(item.type, slot);
                if (!resource)
                    continue;
                auto* texture = static_cast<nvrhi::ITexture*>(resource);
                auto* buffer = static_cast<nvrhi::IBuffer*>(resource);
                switch (item.type) {
                    case nvrhi::ResourceType::Texture_SRV:
                        placeholder = nvrhi::BindingSetItem::Texture_SRV(item.slot, texture);
                        break;
                    case nvrhi::ResourceType::Texture_UAV:
                        placeholder = nvrhi::BindingSetItem::Texture_UAV(item.slot, texture);
                        break;
                    case nvrhi::ResourceType::TypedBuffer_SRV:
                        placeholder = nvrhi::BindingSetItem::TypedBuffer_SRV(item.slot, buffer);
                        break;
                    case nvrhi::ResourceType::TypedBuffer_UAV:
                        placeholder = nvrhi::BindingSetItem::TypedBuffer_UAV(item.slot, buffer);
                        break;
                    case nvrhi::ResourceType::StructuredBuffer_SRV:
                        placeholder = nvrhi::BindingSetItem::StructuredBuffer_SRV(item.slot, buffer);
                        break;
                    case nvrhi::ResourceType::StructuredBuffer_UAV:
                        placeholder = nvrhi::BindingSetItem::StructuredBuffer_UAV(item.slot, buffer);
                        break;
                    case nvrhi::ResourceType::RawBuffer_SRV:
                        placeholder = nvrhi::BindingSetItem::RawBuffer_SRV(item.slot, buffer);
                        break;
                    case nvrhi::ResourceType::RawBuffer_UAV:
                        placeholder = nvrhi::BindingSetItem::RawBuffer_UAV(item.slot, buffer);
                        break;
                    case nvrhi::ResourceType::ConstantBuffer:
                        placeholder = nvrhi::BindingSetItem::ConstantBuffer(item.slot, buffer);
                        break;
                    case nvrhi::ResourceType::Sampler:
                        placeholder = nvrhi::BindingSetItem::Sampler(item.slot, static_cast<nvrhi::ISampler*>(resource));
                        break;
                    default:
                        continue;
                }
            }
            placeholder.arrayElement = element;
            result.bindings.push_back(placeholder);
        }
    }
    return result;
}

nvrhi::BindingSetHandle BindingSetFactory::Create(const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout) {
    nvrhi::BindingSetHandle bindingSet;
    m_Device->createBindingSet(Complete(desc, layout), layout, &bindingSet);
    return bindingSet;
}

nvrhi::BindingSetHandle BindingSetFactory::GetOrCreate(donut::engine::BindingCache& cache,
                                                       const nvrhi::BindingSetDesc& desc,
                                                       nvrhi::IBindingLayout* layout) {
    return cache.GetOrCreateBindingSet(Complete(desc, layout), layout);
}

VoxelRenderer::VoxelRenderer(nvrhi::IDevice* device, donut::engine::ShaderFactory* shaderFactory)
    : m_Device(device), m_ShaderFactory(shaderFactory), m_BindingSetFactory(device) {
}

VoxelRenderer::~VoxelRenderer() {
    for (auto& tracer : m_ViewTracers) {
        nvrhi::SafeRelease(tracer);
    }
    m_ViewTracers.clear();
}

Status VoxelRenderer::setVoxelizationParameters(nvrhi::ICommandList* commandList,
                                                const VoxelizationParameters& params, bool *invalidated) {
    Status rc;
    DerivedVoxelizationParameters derivedParams;

    *invalidated = false;

    if ((VoxelizationParameters&)m_Parameters == params)
        return Status::OK;

    if ((rc = TranslateVoxelizationParameters(params, derivedParams)) != Status::OK)
        return rc;

    if(derivedParams != m_Parameters) {
        ReleaseResources();
        m_Parameters = derivedParams;
        rc = AllocateResources(commandList);

        *invalidated = true;
    }
    return rc;
}

Status VoxelRenderer::createNewTracer(ViewTracer** ppViewTracer, bool ambientOcclusion) {
    Status rc = Status::OK;

    auto tracer = MAKE_RC_OBJ_PTR(ViewTracer, this, ambientOcclusion);

    if (ppViewTracer) {
        *ppViewTracer = tracer;
        tracer->AddRef();
        m_ViewTracers.push_back(tracer);
        tracer->AddRef();
    }
    return rc;
}

const DerivedVoxelizationParameters* VoxelRenderer::GetVoxelizationParameters() {
    return &m_Parameters;
}

nvrhi::IDevice* VoxelRenderer::GetDevice() { return m_Device; }

donut::engine::ShaderFactory* VoxelRenderer::GetShaderFactory() { return m_ShaderFactory; }

uint VoxelRenderer::GetNumFramesInFlight() { return 3; }

PerGPUData* VoxelRenderer::GetCurrentPerGPUData() { return &m_PerGPUData; }

const ClipmapGeometry* VoxelRenderer::GetClipmapGeometry() { return &m_ClipGeometry; }

AllocationMap* VoxelRenderer::GetAllocationMap() { return m_AllocationMap; }

// Records the initialization of the new resources on the caller's command list. This used to open a command
// list of its own, but it is called from inside the frame's command list: two immediate command lists open
// at once, which on D3D11 means two nvrhi command lists driving the one immediate context.
Status VoxelRenderer::AllocateResources(nvrhi::ICommandList* commandList) {
    Status rc;
    auto params = GetVoxelizationParameters();

    m_ClipGeometry.resize(m_Parameters.totalLevels);
    m_FullRevoxelizationRequired = true;

    {
        m_FullScreenQuadVS = m_ShaderFactory->CreateShader("app/UserDefined/FullScreenQuadVS.hlsl", "main", nullptr,
                                           nvrhi::ShaderType::Vertex);
        if (!m_FullScreenQuadVS)
            return Status::RESOURCE_CREATION_FAILED;
    }

    m_AllocationMap = nvrhi::MakeMono<AllocationMap>(this);
    if(VXGI_FAILED(rc = m_AllocationMap->AllocateResources()))
        return rc;

    m_VoxelTexture = nvrhi::MakeMono<VoxelTexture>(this);
    if(VXGI_FAILED(rc = m_VoxelTexture->AllocateResources(commandList)))
        return rc;

    // User defined
    {
        nvrhi::BindingLayoutDesc bindingLayoutDesc;
        bindingLayoutDesc.visibility = nvrhi::ShaderType::All;
        bindingLayoutDesc.registerSpaceIsDescriptorSet = true;
        bindingLayoutDesc.registerSpace = VXGI_RESOURCE_SPACE;
        bindingLayoutDesc.bindings = {
            nvrhi::BindingLayoutItem::ConstantBuffer(
                VXGI_VOXELIZE_CB_SLOT),  // VoxelizationCB
            nvrhi::BindingLayoutItem::ConstantBuffer(
                VXGI_VOXELIZE_MATERIAL_CB_SLOT),  // VoxelizationMaterialCB
            nvrhi::BindingLayoutItem::Texture_SRV(
                VXGI_INVALIDATE_BITMAP_SRV_SLOT),  // t_InvalidateBitmap
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(
                VXGI_COVERAGE_MASKS_SRV_SLOT),  // t_VoxelizationCoverageMasks
            nvrhi::BindingLayoutItem::Texture_SRV(
                VXGI_IRRADIANCE_MAP_SRV_SLOT),  // t_IrradianceMap
            nvrhi::BindingLayoutItem::Texture_UAV(
                VXGI_ALLOCATION_MAP_UAV_SLOT),  // u_AllocationMap
            nvrhi::BindingLayoutItem::Texture_UAV(
                VXGI_COVERAGE_POS_UAV_SLOT),  // u_CoverageTextureXYZ_Pos
            nvrhi::BindingLayoutItem::Texture_UAV(
                VXGI_COVERAGE_NEG_UAV_SLOT),  // u_CoverageTextureXYZ_Neg
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(
                VXGI_SCISSOR_STATS_UAV_SLOT),  // u_ScissorStats
            nvrhi::BindingLayoutItem::Texture_UAV(
                VXGI_EMITTANCE_EVEN_R_UAV_SLOT),  // u_EmittanceEvenR
            nvrhi::BindingLayoutItem::Texture_UAV(
                VXGI_EMITTANCE_EVEN_G_UAV_SLOT),  // u_EmittanceEvenG
            nvrhi::BindingLayoutItem::Texture_UAV(
                VXGI_EMITTANCE_EVEN_B_UAV_SLOT),  // u_EmittanceEvenB
            nvrhi::BindingLayoutItem::Texture_UAV(
                VXGI_EMITTANCE_ODD_R_UAV_SLOT),  // u_EmittanceOddR
            nvrhi::BindingLayoutItem::Texture_UAV(
                VXGI_EMITTANCE_ODD_G_UAV_SLOT),  // u_EmittanceOddG
            nvrhi::BindingLayoutItem::Texture_UAV(
                VXGI_EMITTANCE_ODD_B_UAV_SLOT),  // u_EmittanceOddB
            nvrhi::BindingLayoutItem::Sampler(VXGI_IRRADIANCE_MAP_SAMPLER_SLOT)
        };
        m_Device->createBindingLayout(bindingLayoutDesc, &m_VXGIBindingLayout);

        std::vector<donut::engine::ShaderMacro> voxelizeGS_Macros = {
            {"EXPLICIT_FAST_GS",                 "0"},
            {"COVERAGE_WITH_EDGE_EQUATIONS",     "0"},
            {"USE_SOFTWARE_CONSERVATIVE_RASTER", "0"},
        };

        bool conservativeRasterization =
            m_Device->queryFeatureSupport(nvrhi::Feature::ConservativeRasterization);

        m_VoxelizeGS =
            m_ShaderFactory->CreateShader("app/UserDefined/VoxelizeGS.hlsl", "main",
                                          &voxelizeGS_Macros, nvrhi::ShaderType::Geometry);
        voxelizeGS_Macros[1].definition = "1";
        voxelizeGS_Macros[2].definition = conservativeRasterization ? "0" : "1";
        m_VoxelizeGS_SWCoverage =
            m_ShaderFactory->CreateShader("app/UserDefined/VoxelizeGS.hlsl", "main",
                                          &voxelizeGS_Macros, nvrhi::ShaderType::Geometry);

        // All three variants carry the same two macros: ShaderTool keys a permutation blob by
        // the exact set of macro names, and Shaders.cfg builds VoxelizePS as one product row
        // (UNORM8 is the only emittance format it builds). The opacity pass does not use the
        // format, so it asks for the UNORM8 permutation whatever the voxel texture format is.
        std::vector<donut::engine::ShaderMacro> voxelizePS_Macros = {
            {"VXGI_PS_TYPE", "0"},
            {"EMITTANCE_FORMAT", "UNORM8"}
        };
        m_VoxelizeOpacityPS =
            m_ShaderFactory->CreateShader("app/UserDefined/VoxelizePS.hlsl", "main",
                                          &voxelizePS_Macros, nvrhi::ShaderType::Pixel);

        voxelizePS_Macros[0].definition = "1";
        voxelizePS_Macros[1] = m_VoxelTexture->GetEmittanceFormatMacroForVoxelization();
        m_VoxelizeEmittancePS =
            m_ShaderFactory->CreateShader("app/UserDefined/VoxelizePS.hlsl", "main",
                                          &voxelizePS_Macros, nvrhi::ShaderType::Pixel);

        voxelizePS_Macros[0].definition = "2";
        m_VoxelizeEmittancePS_SWCoverage =
            m_ShaderFactory->CreateShader("app/UserDefined/VoxelizePS.hlsl", "main",
                                          &voxelizePS_Macros, nvrhi::ShaderType::Pixel);
    }

    nvrhi::BufferDesc bufDesc;
    bufDesc.isConstantBuffer = true;
    bufDesc.isVolatile = true;
    bufDesc.byteSize = sizeof(AbstractTracingConstants);
    bufDesc.maxVersions = GetNumFramesInFlight();
    m_Device->createBuffer(bufDesc, &m_pAbstractTracingCB);

    bufDesc.byteSize = sizeof(CacheLevelConstants);
    m_Device->createBuffer(bufDesc, &m_pCacheLevelsCB);

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    samplerDesc.mipFilter = false;
    m_Device->createSampler(samplerDesc, &m_pSamplerLinearWrap);

    for (auto tracer : m_ViewTracers) {
        if (VXGI_FAILED(rc = tracer->AllocateResources(commandList)))
            return rc;
    }

    return Status::OK;
}

void VoxelRenderer::ReleaseResources() {
    Status rc;
    for(auto pViewTracer : m_ViewTracers) {
        rc = pViewTracer->ReleaseResources();
        NVRHI_ASSERT(VXGI_SUCCEEDED(rc));
    }

    m_VoxelTexture = nullptr;
    m_AllocationMap = nullptr;
    m_FullScreenQuadVS = nullptr;
    m_pAbstractTracingCB = nullptr;
    m_pCacheLevelsCB = nullptr;
    m_pSamplerLinearWrap = nullptr;
}

Status VoxelRenderer::TranslateVoxelizationParameters(const VoxelizationParameters& params,
                                                 DerivedVoxelizationParameters& derived) {
    uint logSize = log2_ceil(params.mapSize);

    if (!(IsPowerOf2(params.mapSize) && params.mapSize >= 0x10 &&
          params.mapSize <= 0x100)) {
        donut::log::fatal(
            "VoxelizationParameters.mapSize (%u) must be a power of 2 and within range "
            "[16, 256]",
            params.mapSize);
        return Status::INVALID_ARGUMENT;
    }

    if (!(params.mipLevels < logSize)) {
        donut::log::fatal(
            "For selected VoxelizationParameters.mapSize (%u), mipLevels (%u) must be "
            "within range [0, %u].",
            params.mapSize, params.mipLevels, logSize - 1);
        return Status::INVALID_ARGUMENT;
    }

    if (!(params.stackLevels && params.stackLevels <= logSize)) {
        donut::log::fatal(
            "For selected VoxelizationParameters.mapSize (%u), stackLevels (%u) must be "
            "within range [1, %u].",
            params.mapSize, params.stackLevels, logSize);
        return Status::INVALID_ARGUMENT;
    }

    uint maxPageSize = params.mapSize / 2;
    uint maxPageLevels = std::min<uint>(log2_ceil(maxPageSize) + 1, 7);
    uint voxelsPerLevel = params.mapSize * params.mapSize * params.mapSize;
    uint minBias = log8_ceil(voxelsPerLevel / 0x3FFFC0); // (0x400000 - 0x30)
    uint maxBias = (uint)std::max<int>(maxPageLevels - params.stackLevels, 0);

    if (!(params.allocationMapLodBias >= minBias &&
          params.allocationMapLodBias <= maxBias)) {
        donut::log::fatal(
            "For selected VoxelizationParameters.mapSize (%u) and stackLevels (%u), "
            "allocationMapLodBias (%u) must be within range [%u, %u].",
            params.mapSize, params.stackLevels, params.allocationMapLodBias, minBias,
            maxBias);
        return Status::INVALID_ARGUMENT;
    }

    OpacityDirections opacityDirectionCount = params.opacityDirectionCount;
    if (!(opacityDirectionCount == OpacityDirections::THREE_DIMENSIONAL ||
          opacityDirectionCount == OpacityDirections::SIX_DIMENSIONAL)) {
        donut::log::fatal(
            "VoxelizationParameters.opacityDirection Count has an invalid value (%d)",
            opacityDirectionCount);
        return Status::INVALID_ARGUMENT;
    }

    if (!(params.emittanceStorageScale > 0.f)) {
        donut::log::fatal(
            "VoxelizationParameters.emittanceStorageScale (%f) must be positive",
            params.emittanceStorageScale);
        return Status::INVALID_ARGUMENT;
    }

    bool isFp16Supported = false;
    EmittanceFormat emittanceFormat;

    switch (params.emittanceFormat) {
        case EmittanceFormat::NONE:
        case EmittanceFormat::UNORM8:
        case EmittanceFormat::FLOAT32:
            emittanceFormat = params.emittanceFormat;
            break;
        case EmittanceFormat::PERFORMANCE:
            emittanceFormat =
                isFp16Supported ? EmittanceFormat::FLOAT16 : EmittanceFormat::UNORM8;
            break;
        case EmittanceFormat::QUALITY:
            emittanceFormat =
                isFp16Supported ? EmittanceFormat::FLOAT16 : EmittanceFormat::FLOAT32;
            break;
        case EmittanceFormat::FLOAT16:
            emittanceFormat = params.emittanceFormat;
            break;
        default:
            donut::log::fatal(
                "VoxelizationParameters.emittanceFormat has an invalid value (%d)",
                params.emittanceFormat);
            return Status::INVALID_ARGUMENT;
    }

    if (emittanceFormat == EmittanceFormat::FLOAT16 && !isFp16Supported) {
        donut::log::fatal("VoxelizationParameters.emittanceFormat does not accept FLOAT16");
        return Status::NOT_SUPPORTED;
    }

    if (params.enableMultiBounce && params.emittanceFormat == EmittanceFormat::NONE) {
        donut::log::fatal(
            "Multi-bounce tracing cannot be performed without emittance (emittanceFormat "
            "== NONE)");
        return Status::INVALID_CONFIGURATION;
    }

    if (!(params.indirectIrradianceMapLodBias > (int)-params.stackLevels &&
          params.indirectIrradianceMapLodBias <= (int)params.allocationMapLodBias)) {
        donut::log::fatal(
            "indirectIrradianceMapLodBias (%d) is out of range, acceptable values are "
            "-stackLevels+1 (%d) ."
            ".. allocationMapLodBias (%d)",
            params.indirectIrradianceMapLodBias, 1 - (int)params.stackLevels,
            (int)params.allocationMapLodBias);
        return Status::INVALID_CONFIGURATION;
    }

    if (!(params.indirectIrradianceMapLodBias >= 0 ||
          ((1 << (-params.indirectIrradianceMapLodBias)) * params.mapSize <= 0x100))) {
        donut::log::fatal(
            "indirectIrradianceMapLodBias (%d) is too small, it requires the irradiance "
            "map to have %d^3 "
            "voxels, while it can't be larger than 256^3",
            params.indirectIrradianceMapLodBias,
            (1 << (-params.indirectIrradianceMapLodBias)) * params.mapSize);
        return Status::INVALID_CONFIGURATION;
    }

    (VoxelizationParameters&)derived = params;
    derived.emittanceFormat = emittanceFormat;
    derived.totalLevels = params.mipLevels + params.stackLevels;
    derived.pageLevels = params.allocationMapLodBias + params.stackLevels;
    derived.useOpacityInterpolation = params.stackLevels > 1;

    int indirectIrradianceMapLod;
    if (params.enableMultiBounce)
        indirectIrradianceMapLod =
            params.stackLevels + params.indirectIrradianceMapLodBias - 1;
    else
        indirectIrradianceMapLod = -1;
    derived.indirectIrradianceMapLod = indirectIrradianceMapLod;

    uint indirectIrradianceMapSize;
    if (params.indirectIrradianceMapLodBias < 0) {
        indirectIrradianceMapSize = params.mapSize
                                    << (-params.indirectIrradianceMapLodBias);
    } else {
        indirectIrradianceMapSize = params.mapSize >> params.indirectIrradianceMapLodBias;
    }
    derived.indirectIrradianceMapSize = indirectIrradianceMapSize;

    derived.allocationMapLod = params.stackLevels + params.allocationMapLodBias - 1;

    derived.allocationMapSize = params.mapSize >> params.allocationMapLodBias;

    derived.enableNvidiaExtensions = false;

    derived.enableGeometryShaderPassthrough =
        isFp16Supported && params.enableGeometryShaderPassthrough;

    return Status::OK;
}

box3 VoxelRenderer::calculateHypotheticalWorldRegion(const float3& clipmapAnchor,
                                                float giRange) {
    float3 center =
        m_ClipGeometry.calculateClipmapCenter(clipmapAnchor, giRange, m_Parameters);
    float size = giRange * float(1 << (m_Parameters.stackLevels - 1));
    box3 result{center - size, center + size};
    return result;
}

Status VoxelRenderer::prepareVoxelizationViewMatrix(const float3& clipmapAnchor, float giRange,
                                               float4x4* viewMatrix) {
    if(!(giRange > 0. && giRange < FLT_MAX)) {
        donut::log::error(
            "giRange (%f) must be positive and represent a reasonable range in world space "
            "units",
            giRange);
        return Status::INVALID_ARGUMENT;
    }

    auto cm = m_ClipGeometry;

    cm.update(clipmapAnchor, giRange, m_Parameters);
    affine3 viewXform;
    viewXform = translation(-cm.m_Center) * scaling(float3(2.f / cm.m_WorldSize));
    *viewMatrix = affineToHomogeneous(viewXform);

    return Status::OK;
}

Status VoxelRenderer::prepareForOpacityVoxelization(nvrhi::ICommandList* commandList,
                                               const UpdateVoxelizationParameters* params,
                                               bool* performOpacityVoxelization,
                                               bool* performEmittanceVoxelization) {
    if (!(maxComponent(params->sceneExtents.diagonal()) > 0)) {
        donut::log::error("sceneExtents is empty");
        return Status::INVALID_ARGUMENT;
    }

    if (!(params->giRange > 0.f && params->giRange < FLT_MAX)) {
        donut::log::error(
            "giRange (%f) must be positive and represent a reasonable range in world space "
            "units",
            params->giRange);
        return Status::INVALID_ARGUMENT;
    }

    m_ClipGeometry.update(params->clipmapAnchor, params->giRange, m_Parameters);
    m_IndirectIrradianceMapTracingParameters =
        params->indirectIrradianceMapTracingParameters;

    auto perGPUData = GetCurrentPerGPUData();
    m_AnchorMoved = false;

    if (any(params->clipmapAnchor != perGPUData->m_ClipmapAnchorPreviousFrame)) {
        perGPUData->m_ClipmapAnchorPreviousFrame = params->clipmapAnchor;
        m_AnchorMoved = true;
    }

    if (perGPUData->m_ClipRangePreviousFrame != params->giRange) {
        perGPUData->m_ClipRangePreviousFrame = params->giRange;
        m_FullRevoxelizationRequired = true;
    }

    m_SceneExtents = params->sceneExtents;

    if (!m_FullRevoxelizationRequired && m_Parameters.persistentVoxelData) {

        auto gpuData = perGPUData;
        gpuData->m_RegionsToInvalidate.reserve(params->invalidatedRegionCount +
                                               gpuData->m_RegionsToInvalidate.size());
        gpuData->m_LightFrustaToInvalidate.reserve(
            params->invalidatedFrustumCount + gpuData->m_LightFrustaToInvalidate.size());
    }

    for (uint i = 0; i < params->invalidatedRegionCount; ++i)
        InvalidateRegion(params->invalidatedRegions[i]);

    for (uint j = 0; j < params->invalidatedFrustumCount; ++j) {
        frustum f = params->invalidatedLightFrusta[j];
        ExtentFrustumForConservativeVoxelization(f, m_ClipGeometry.m_PageSize);
        InvalidateLightFrustum(f);
    }

    m_AllocationMap->SetPerGpuAlternatingFrameIndex(perGPUData->m_AlternatingFrameIndex);
    perGPUData->m_AlternatingFrameIndex ^= 1;

    if(m_Parameters.persistentVoxelData) {
        if(m_FullRevoxelizationRequired) {
            InvalidateRegion(m_ClipGeometry.m_WorldRegion);
            m_FullRevoxelizationRequired = false;
        }

        for (uint level = 0; level < m_Parameters.stackLevels; ++level) {
            InvalidateLevelMovementSlabs(level, m_ClipGeometry.m_LevelRegions[level]);
        }
    }

    m_SnappedRegionsToInvalidate.clear();

    m_UpdateOpacity =
        !m_Parameters.persistentVoxelData || !perGPUData->m_RegionsToInvalidate.empty();

    m_UpdateEmittance = m_Parameters.emittanceFormat != EmittanceFormat::NONE &&
                        (!m_Parameters.persistentVoxelData || m_UpdateOpacity ||
                         !perGPUData->m_LightFrustaToInvalidate.empty());

    if(m_UpdateOpacity || m_AnchorMoved || m_UpdateEmittance) {
        commandList->beginMarker("AMap: Invalidation");
        m_AllocationMap->SnapAndInvalidateRegions(
            commandList, perGPUData->m_RegionsToInvalidate,
            perGPUData->m_LightFrustaToInvalidate, m_SnappedRegionsToInvalidate,
            m_AnchorMoved);
        commandList->endMarker();

        if(m_AnchorMoved && perGPUData->m_OpacityCleared) {
            commandList->beginMarker("Opacity: Restore pages");
            m_VoxelTexture->RestoreOpacityPages(commandList);
            commandList->endMarker();
        }

        m_VoxelTexture->FillListProcessingBuffers(commandList);

        if(m_UpdateOpacity) {
            commandList->beginMarker("Opacity: Clear pages");
            m_VoxelTexture->ClearOpacityPages(commandList);
            commandList->endMarker();
        }

        if(m_UpdateEmittance) {
            commandList->beginMarker("Emittance: Clear pages");
            m_VoxelTexture->ClearEmittancePages(commandList);
            commandList->endMarker();
        }

        box3 scissor = box3::empty();
        for(auto &region : m_SnappedRegionsToInvalidate) {
            scissor |= region;
        }

        m_VoxelTexture->InvalidateVoxelizationRenderState();
        uint numMaxViewports = nvrhi::c_MaxViewports;
        uint numViewports;
        m_VoxelizeViewports.resize(numMaxViewports);
        m_VoxelizeScissorRects.resize(numMaxViewports);
        m_VoxelTexture->prepareVoxelizationRenderState(
            scissor, false, MaterialSamplingRate::FIXED_DEFAULT, numMaxViewports,
            &numViewports,
            m_VoxelizeViewports.data(),
            m_VoxelizeScissorRects.data());
        m_VoxelizeViewports.resize(numViewports);
        m_VoxelizeScissorRects.resize(numViewports);
        m_VoxelTexture->ClearStatistics(commandList);
    }

    *performOpacityVoxelization = m_UpdateOpacity;
    *performEmittanceVoxelization = m_UpdateEmittance;

    return Status::OK;
}

Status VoxelRenderer::prepareForEmittanceVoxelization() {
    m_VoxelTexture->InvalidateVoxelizationRenderState();
    return Status::OK;
}

Status VoxelRenderer::getInvalidatedRegion(box3* pRegions, uint maxRegions, uint* numRegions) {
    *numRegions = (uint)m_SnappedRegionsToInvalidate.size();
    if(pRegions || !maxRegions) {
        if(*numRegions < maxRegions) {
            if(*numRegions)
                memcpy(pRegions, m_SnappedRegionsToInvalidate.data(),
                       *numRegions * sizeof(box3));
            return Status::OK;
        } else {
            return Status::BUFFER_TOO_SMALL;
        }
    } else {
        return Status::NULL_ARGUMENT;
    }
}

float VoxelRenderer::getMinVoxelSizeAtPoint(const float3& point, VoxelSizeFunction function,
                                       bool zeroOutOfRange) {
    float3 centerVector = point - m_ClipGeometry.m_Center;
    centerVector = abs(centerVector);
    float maxC = maxComponent(centerVector);
    float maxDistance = m_ClipGeometry.m_WorldSize * 0.5f;
    float distance = maxC / maxDistance;
    if(distance > 1.f) {
        if(zeroOutOfRange)
            return 0.f;
        distance = 1.f;
    }

    if(function != VoxelSizeFunction::EXACT) {
        if(function == VoxelSizeFunction::LINEAR_UNDERESTIMATE) {
            return std::max(
                distance * m_ClipGeometry.m_VoxelSizes[m_Parameters.stackLevels - 1],
                m_ClipGeometry.m_VoxelSizes[0]);
        } else {
            return std::max(
                distance * m_ClipGeometry.m_VoxelSizes[m_Parameters.stackLevels - 1] * 2.f,
                m_ClipGeometry.m_VoxelSizes[0]);
        }
    } else {
        float level = std::max((std::log2(distance) + m_Parameters.stackLevels) - 1.f, 0.f);
        level = std::ceil(level);
        if(level >= 0.f && level < m_Parameters.stackLevels)
            return m_ClipGeometry.m_VoxelSizes[(int)level];
        else
            return 0.f;
    }
}

float3 VoxelRenderer::getLastUpdatedClipmapAnchor() {
    return m_ClipGeometry.m_Anchor;
}

void VoxelRenderer::InvalidateRegion(const box3& boundingBox) {
    if(maxComponent(boundingBox.diagonal()) > 0.f) {
        auto gpuData = &m_PerGPUData;
        gpuData->m_RegionsToInvalidate.push_back(boundingBox);
    }
}

void VoxelRenderer::InvalidateRegionForCurrentGPU(const box3& boundingBox) {
    return InvalidateRegion(boundingBox);
}

void VoxelRenderer::InvalidateLevelMovementSlabs(uint level, const box3& newRegion) {
    auto& prevLevelRegions = GetCurrentPerGPUData()->m_PreviousLevelRegions;
    if(prevLevelRegions.size() <= level)
        prevLevelRegions.resize(level + 1, box3::empty());

    auto prevRegion = prevLevelRegions[level];
    if(maxComponent(prevRegion.diagonal()) <= 0.f) {
        prevLevelRegions[level] = newRegion;
        return;
    }

    if (any(prevRegion.lower() != newRegion.lower()) ||
        any(prevRegion.upper() != newRegion.upper())) {
        if(prevRegion.intersects(newRegion)) {
            if (newRegion.lower().x <= prevRegion.lower().x) {
                if (newRegion.lower().x < prevRegion.lower().x) {
                    InvalidateRegion(box3{newRegion.lower(),
                                          float3(prevRegion.lower().x, newRegion.upper().y,
                                                 newRegion.upper().z)});
                }
            } else {
                InvalidateRegion(box3{
                    float3(prevRegion.upper().x, newRegion.lower().y, newRegion.lower().z),
                    newRegion.upper()});
            }

            if(newRegion.lower().y <= prevRegion.lower().y) {
                if (newRegion.lower().y < prevRegion.lower().y) {
                    InvalidateRegion(box3{
                        newRegion.lower(),
                        float3(newRegion.upper().x, prevRegion.lower().y,
                               newRegion.upper().z),
                    });
                }
            } else {
                InvalidateRegion(box3{
                    float3(newRegion.lower().x, prevRegion.upper().y, newRegion.lower().z),
                    newRegion.upper()});
            }

            if (newRegion.lower().z <= prevRegion.lower().z) {
                if (newRegion.lower().z < prevRegion.lower().z) {
                    InvalidateRegion(box3{newRegion.lower(),
                                          float3(newRegion.upper().x, newRegion.upper().y,
                                                 prevRegion.lower().z)});
                }
            } else {
                InvalidateRegion(box3{
                    float3(newRegion.lower().x, newRegion.lower().y, prevRegion.upper().z),
                    newRegion.upper()});
            }
        } else {
            InvalidateRegion(newRegion);
        }

        prevLevelRegions[level] = newRegion;
    }
}

void VoxelRenderer::FillAbstractTracingConstants(nvrhi::ICommandList* commandList) {
    AbstractTracingConstants abstractConstants = {};

    float3 sceneMargin = m_SceneExtents.diagonal() * 0.1f;
    uint3 textureSize;

    textureSize = m_VoxelTexture->GetOpacityTextureSize();
    abstractConstants.m_vrOpacityTextureSize = float4(1.f / float3(textureSize), 0.f);

    textureSize = m_VoxelTexture->GetEmittanceTextureSize();
    abstractConstants.m_vrEmittanceTextureSize = float4(1.f / float3(textureSize), 0.f);

    float NearestLevelBoundary =
        m_ClipGeometry.getNearestLevelBoundary(m_Parameters.stackLevels - 1);
    abstractConstants.m_vClipmapAnchor =
        float4(m_ClipGeometry.m_Anchor, NearestLevelBoundary);

    abstractConstants.m_vSceneBoundaryLower =
        float4(m_SceneExtents.lower() - sceneMargin, 0.f);
    abstractConstants.m_vSceneBoundaryUpper =
        float4(m_SceneExtents.upper() + sceneMargin, 0.f);
    abstractConstants.m_vClipmapCenter = float4(m_ClipGeometry.m_Center, 1.f / m_ClipGeometry.m_LevelSizes[0]);

    abstractConstants.m_vToroidalOffset = float4(
        float3(m_ClipGeometry.m_ClipmapToroidalOffsets[m_Parameters.stackLevels - 1]) /
            float(m_Parameters.mapSize),
        0.f);

    abstractConstants.m_fFinestVoxelSize = m_ClipGeometry.m_VoxelSizes[0];
    abstractConstants.m_fStackTextureSize = (float)m_Parameters.mapSize;

    abstractConstants.m_frNearestLevel0Boundary =
        m_Parameters.stackLevels == 1 ? 0.f
                                      : 1.f / m_ClipGeometry.getNearestLevelBoundary(0);

    abstractConstants.m_fMaxMipmapLevel = float(m_Parameters.totalLevels - 1);

    abstractConstants.m_frEmittanceStorageScale =
        1.f / m_VoxelTexture->GetEmittanceStorageScale();

    abstractConstants.m_frClipmapSizeWorld = 1.f / m_ClipGeometry.m_WorldSize;
    abstractConstants.m_bUse6DOpacity =
        m_VoxelTexture->GetOpacityDirectionCount() == OpacityDirections::SIX_DIMENSIONAL;
    float rcpEmittanceTextureSizeX = 1.f / float(m_VoxelTexture->GetEmittanceTextureSize().x);
    int PackingStride = m_VoxelTexture->GetPackingStride();
    abstractConstants.m_fEmittancePackingStride = float(PackingStride) * rcpEmittanceTextureSizeX;
    commandList->writeBuffer(m_pAbstractTracingCB, &abstractConstants, sizeof(abstractConstants));

    CacheLevelConstants levelConstants = {};
    memset(&levelConstants, 0, sizeof(levelConstants));
    for (int level = 0; level < m_Parameters.totalLevels; ++level) {
        int maxClipLevel = m_Parameters.stackLevels - 1;
        float4 translationParameter;
        translationParameter.x =
            std::pow(0.5f, std::min(level, maxClipLevel)) * abstractConstants.m_vClipmapCenter.w;
        translationParameter.y =
            std::pow(0.5f, std::max(level - maxClipLevel, 0)) * abstractConstants.m_fStackTextureSize;
        translationParameter.z = m_VoxelTexture->GetLodPackingOffset(level, 0);
        translationParameter.w = m_VoxelTexture->GetLodPackingOffset(level, 1);

        levelConstants.m_TranslationParameters[level] = translationParameter;

        float4 translationParameter2 =
            abstractConstants.m_vToroidalOffset * std::pow(2.f, (float)std::max(0, maxClipLevel - level));
        levelConstants.m_TranslationParameters2[level] = float4(translationParameter2.xyz(), 0.f);
    }
    commandList->writeBuffer(m_pCacheLevelsCB, &levelConstants, sizeof(levelConstants));
}

void VoxelRenderer::InvalidateLightFrustum(const frustum& f) {
    auto gpuData = &m_PerGPUData;
    gpuData->m_LightFrustaToInvalidate.push_back(f);
}

void VoxelRenderer::GetOpacityShaders(nvrhi::IShader** geometryShader,
                                 nvrhi::IShader** pixelShader) {
    *geometryShader = m_VoxelizeGS;
    *pixelShader = m_VoxelizeOpacityPS;
    (*geometryShader)->AddRef();
    (*pixelShader)->AddRef();
}

void VoxelRenderer::GetEmittanceShaders(bool superSampling, nvrhi::IShader** geometryShader,
                                   nvrhi::IShader** pixelShader) {
    if (superSampling) {
        *geometryShader = m_VoxelizeGS_SWCoverage;
        *pixelShader = m_VoxelizeEmittancePS_SWCoverage;
    } else {
        *geometryShader = m_VoxelizeGS;
        *pixelShader = m_VoxelizeEmittancePS;
    }

    (*geometryShader)->AddRef();
    (*pixelShader)->AddRef();
}

void VoxelRenderer::GetBindingLayout(nvrhi::IBindingLayout** bindingLayout) {
    *bindingLayout = m_VXGIBindingLayout;
    (*bindingLayout)->AddRef();
}

nvrhi::IBuffer* VoxelRenderer::GetAbstractTracingCB() { return m_pAbstractTracingCB; }

nvrhi::IBuffer* VoxelRenderer::GetTracingClipmapLevelCB() { return m_pCacheLevelsCB; }

nvrhi::ISampler* VoxelRenderer::GetLinearWrapSampler() { return m_pSamplerLinearWrap; }

VoxelTexture* VoxelRenderer::GetVoxelTexture() { return m_VoxelTexture; }

nvrhi::IShader* VoxelRenderer::GetFullScreenQuadVS() { return m_FullScreenQuadVS; }

nvrhi::IFramebuffer* VoxelRenderer::GetProjectedCoverageFramebuffer() {
    return m_VoxelTexture->GetProjectedCoverageFramebuffer();
}

uint VoxelRenderer::GetUserShaderBindingSlot(UserShaderBindingID id) {
  static const uint ShaderBindingIDMap[][2] = {
    // clang-format off
        {(uint)UserShaderBindingID::UNKNOWN,                0u},
        {(uint)UserShaderBindingID::VOXELIZE_MATERIAL_CB,   VXGI_VOXELIZE_MATERIAL_CB_SLOT  },
        {(uint)UserShaderBindingID::VOXELIZE_CB,            VXGI_VOXELIZE_CB_SLOT           },
        {(uint)UserShaderBindingID::SCISSOR_REGIONS_SRV,    VXGI_SCISSOR_REGIONS_SRV_SLOT   },
        {(uint)UserShaderBindingID::IRRADIANCE_MAP_SRV,     VXGI_IRRADIANCE_MAP_SRV_SLOT    },
        {(uint)UserShaderBindingID::INVALIDATE_BITMAP_SRV,  VXGI_INVALIDATE_BITMAP_SRV_SLOT },
        {(uint)UserShaderBindingID::COVERAGE_MASKS_SRV,     VXGI_COVERAGE_MASKS_SRV_SLOT    },
        {(uint)UserShaderBindingID::ALLOCATION_MAP_UAV,     VXGI_ALLOCATION_MAP_UAV_SLOT    },
        {(uint)UserShaderBindingID::EMITTANCE_EVEN_R_UAV,   VXGI_EMITTANCE_EVEN_R_UAV_SLOT  },
        {(uint)UserShaderBindingID::EMITTANCE_EVEN_G_UAV,   VXGI_EMITTANCE_EVEN_G_UAV_SLOT  },
        {(uint)UserShaderBindingID::EMITTANCE_EVEN_B_UAV,   VXGI_EMITTANCE_EVEN_B_UAV_SLOT  },
        {(uint)UserShaderBindingID::EMITTANCE_ODD_R_UAV,    VXGI_EMITTANCE_ODD_R_UAV_SLOT   },
        {(uint)UserShaderBindingID::EMITTANCE_ODD_G_UAV,    VXGI_EMITTANCE_ODD_G_UAV_SLOT   },
        {(uint)UserShaderBindingID::EMITTANCE_ODD_B_UAV,    VXGI_EMITTANCE_ODD_B_UAV_SLOT   },
        {(uint)UserShaderBindingID::COVERAGE_POS_UAV,       VXGI_COVERAGE_POS_UAV_SLOT      },
        {(uint)UserShaderBindingID::COVERAGE_NEG_UAV,       VXGI_COVERAGE_NEG_UAV_SLOT      },
        {(uint)UserShaderBindingID::SCISSOR_STATS_UAV,      VXGI_SCISSOR_STATS_UAV_SLOT     },
        {(uint)UserShaderBindingID::IRRADIANCE_MAP_SAMPLER, VXGI_IRRADIANCE_MAP_SAMPLER_SLOT},
    // clang-format on
    };
  int index = (int)((uint)id - (uint)UserShaderBindingID::UNKNOWN);

  NVRHI_ASSERT((index >= 0 && index < std::size(ShaderBindingIDMap)));
  return ShaderBindingIDMap[index][1];
}

Status VoxelRenderer::getVoxelizationViewMatrix(float4x4* viewMatrix) {
    affine3 translateXform, scaleXform;
    translateXform = dm::translation(-m_ClipGeometry.m_Center);
    scaleXform = dm::scaling(float3{2.f / m_ClipGeometry.m_WorldSize});
    affine3 viewXform = translateXform * scaleXform;
    *viewMatrix = affineToHomogeneous(viewXform);
    return Status::OK;
}

Status VoxelRenderer::getVoxelizationState(nvrhi::ICommandList *commandList, bool isEmittance, const MaterialInfo& materialInfo,
                                      nvrhi::GraphicsState* state) {
    Status rc;

    if (isEmittance) {
        if(materialInfo.materialSamplingRate > MaterialSamplingRate::ADAPTIVE_GE4) {
            donut::log::error("Invalid value of materialInfo.materialSamplingRate (%d)",
                              materialInfo.materialSamplingRate);
            return Status::INVALID_ARGUMENT;
        }

        m_VoxelTexture->InvalidateVoxelizationRenderState();
        box3 scissor = m_ClipGeometry.m_WorldRegion;
        uint numMaxViewports = nvrhi::c_MaxViewports;
        uint numViewports;
        m_VoxelizeViewports.resize(numMaxViewports);
        m_VoxelizeScissorRects.resize(numMaxViewports);
        m_VoxelTexture->prepareVoxelizationRenderState(
            scissor, true, materialInfo.materialSamplingRate, numMaxViewports,
            &numViewports, m_VoxelizeViewports.data(), m_VoxelizeScissorRects.data());
        m_VoxelizeViewports.resize(numViewports);
        m_VoxelizeScissorRects.resize(numViewports);
    }

    const uint numViewports = m_VoxelizeViewports.size();
    state->viewport.viewports.resize(m_VoxelizeViewports.size());
    memcpy(state->viewport.viewports.data(), m_VoxelizeViewports.data(),
           sizeof(nvrhi::Viewport) * numViewports);
    state->viewport.scissorRects.resize(numViewports);
    memcpy(state->viewport.scissorRects.data(), m_VoxelizeScissorRects.data(),
           sizeof(nvrhi::Rect) * numViewports);

    auto perGPUData = GetCurrentPerGPUData();
    nvrhi::BindingSetHandle VXGIBindingSet;
    rc = m_VoxelTexture->getVoxelizationState(
        commandList, isEmittance, materialInfo,
        perGPUData->m_VoxelizationGridCenterPreviousFrame, m_VXGIBindingLayout,
        &VXGIBindingSet);
    int availIndex = -1;
    for (int i = 0; i < state->bindings.size(); ++i) 
        if(state->bindings[i] == VXGIBindingSet) {
            availIndex = i;
            break;
        }

    if(availIndex == -1)
        state->bindings.push_back(VXGIBindingSet);

    return rc;
}

Status VoxelRenderer::updateVoxelizationMaterialParameters(nvrhi::ICommandList* commandList,
                                                      const MaterialInfo& materialInfo) {
    m_VoxelTexture->updateVoxelizationMaterialParameters(commandList, materialInfo);
    return Status::OK;
}

Status VoxelRenderer::finalizeVoxelization(nvrhi::ICommandList* commandList) {

    auto perGPUData = GetCurrentPerGPUData();

    perGPUData->m_RegionsToInvalidate.clear();
    perGPUData->m_LightFrustaToInvalidate.clear();

    if(m_UpdateEmittance && m_Parameters.useHighQualityEmittanceDownsampling) {
        commandList->beginMarker("AMap: Dilate Pages");
        m_AllocationMap->DilateEmissivePages(commandList);
        commandList->endMarker();
    }

    if(m_UpdateOpacity || m_UpdateEmittance || m_AnchorMoved) {
        commandList->beginMarker("AMap: Generate Page Lists");
        m_AllocationMap->GeneratePageLists(commandList);
        commandList->endMarker();
    }

    if(m_UpdateOpacity || m_AnchorMoved)
        m_VoxelTexture->ProcessOpacity(commandList);

    if(m_UpdateEmittance) {
        commandList->beginMarker("Emittance: Process Emissive Objects");
        m_VoxelTexture->ConvertEmissivePages(commandList);
        commandList->endMarker();

        commandList->beginMarker("Emittance: Downsampling");
        m_VoxelTexture->DownsampleEmittancePages(commandList);
        commandList->endMarker();

        commandList->beginMarker("Emittance: Mipmaps Generation");
        m_VoxelTexture->GenerateEmittanceMipmaps(commandList);
        commandList->endMarker();
    }

    if(m_UpdateOpacity || m_AnchorMoved) {
        commandList->beginMarker("Opacity: Wrapping");
        m_VoxelTexture->WrapOpacity(commandList);
        commandList->endMarker();
    }

    FillAbstractTracingConstants(commandList);

    if(m_UpdateEmittance) {
        commandList->beginMarker("Indirect Irradiance Map Tracing");
        m_VoxelTexture->TraceIrradianceMap(commandList, &m_IndirectIrradianceMapTracingParameters);
        commandList->endMarker();
    }

    perGPUData->m_ToroidalOffsetPreviousFrame =
        m_ClipGeometry.m_AllocationMapToroidalOffset;

    perGPUData->m_VoxelizationGridCenterPreviousFrame = float4(m_ClipGeometry.m_Center, 1.f / m_ClipGeometry.m_WorldSize);

    return Status::OK;
}

Status VoxelRenderer::renderDebug(nvrhi::ICommandList *commandList, const DebugRenderParameters* params) {
    Status rc = Status::OK;

    if(params->debugMode == DebugRenderMode::DISABLED)
        return rc;

    {
        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(params->destinationTexture);
        fbDesc.depthAttachment = {params->destinationDepth};
        InvalidateFramebuffer(m_Device, fbDesc, m_DebugFramebuffer.GetAddressOf());
        NVRHI_ASSERT(m_DebugFramebuffer);
    }

    nvrhi::GraphicsState state;
    state.framebuffer = m_DebugFramebuffer;
    state.viewport.addViewport(params->viewport);

    float4x4 viewMatrixInv = inverse(params->viewMatrix);
    float3 cameraPos = viewMatrixInv[3];
    float4x4 viewProjMatrix = params->viewMatrix * params->projMatrix;

    switch (params->debugMode) {
        case DebugRenderMode::ALLOCATION_MAP:
        if(params->level < m_Parameters.totalLevels) {
            m_AllocationMap->RenderDebug(commandList, state, viewProjMatrix, params->nearClipZ,
                                         params->farClipZ, cameraPos, params->bitToDisplay,
                                         params->voxelsToSkip, params->targetOpacity);
        }
        break;
        case DebugRenderMode::OPACITY_TEXTURE:
            if (params->level >= m_Parameters.totalLevels) {
                uint numLevels = std::min(params->level - m_Parameters.totalLevels, m_Parameters.totalLevels);
                for (uint i = numLevels; i; --i) {
                    m_VoxelTexture->RenderDebugOpacity(commandList, state, viewProjMatrix, params->nearClipZ,
                                                       params->farClipZ, cameraPos, i - 1, params->voxelsToSkip,
                                                       true, params->targetOpacity);
                }
            } else {
                m_VoxelTexture->RenderDebugOpacity(commandList, state, viewProjMatrix, params->nearClipZ,
                                                   params->farClipZ, cameraPos, params->level,
                                                   params->voxelsToSkip, false, params->targetOpacity);
            }
            break;
        case DebugRenderMode::EMITTANCE_TEXTURE:
            if (params->level >= m_Parameters.totalLevels) {
                uint numLevels = std::min(params->level - m_Parameters.totalLevels, m_Parameters.totalLevels);
                for (uint i = numLevels; i; --i) {
                    m_VoxelTexture->RenderDebugEmittance(commandList, state, viewProjMatrix, params->nearClipZ,
                                                       params->farClipZ, cameraPos, i - 1, params->voxelsToSkip,
                                                       true, params->targetOpacity);
                }
            } else {
                m_VoxelTexture->RenderDebugEmittance(commandList, state, viewProjMatrix, params->nearClipZ,
                                                   params->farClipZ, cameraPos, params->level,
                                                   params->voxelsToSkip, false, params->targetOpacity);
            }
            break;
        case DebugRenderMode::INDIRECT_IRRADIANCE_TEXTURE:
            if(params->level < m_Parameters.totalLevels) {
                m_VoxelTexture->RenderDebugIrradiance(commandList, state, viewProjMatrix, params->nearClipZ,
                                                      params->farClipZ, cameraPos, params->voxelsToSkip,
                                                      params->targetOpacity);
            }
            break;
        default:
            donut::log::error("Invalid debugMode (%d)", (int)params->debugMode);
            return Status::INVALID_ARGUMENT;
    }

    return rc;
}

const box3& VoxelRenderer::getLastUpdatedWorldRegion() { return m_ClipGeometry.m_WorldRegion; }

const box3& VoxelRenderer::getLastUpdatedSceneExtents() { return m_SceneExtents; }

}  // namespace vxgi
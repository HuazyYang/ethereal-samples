#include "AllocationMap.h"
#include "VoxelRenderer.h"
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/BindingCache.h>

namespace vxgi {

MultiListParams MultiPageList::getListParams(uint level) { MultiListParams params;
    params.segmentOffset = offsets[level];
    params.maxElements = pageCounts[level];
    params.logPageSize = logPageSize0 - level;
    params.dummy = 0;

    return params;
}

uint MultiPageList::getNumLevels() { return (uint)pageCounts.size(); }

size_t MultiPageList::getCounterOffset(uint level) { return 4 * offsets[level]; }

size_t MultiPageList::getDispatchArgumentsOffset(uint level) {
    return 4 * (offsets[level] + 1);
}

AllocationMap::AllocationMap(VoxelRenderer* parent): m_Parent(parent) {
    m_Device = m_Parent->GetDevice();
    m_ShaderFactory = m_Parent->GetShaderFactory();

    m_BindingCache = nvrhi::MakeMono<donut::engine::BindingCache>(m_Device);
}

AllocationMap::~AllocationMap() {}

const DerivedVoxelizationParameters* AllocationMap::GetVoxelizationParameters() {
    return m_Parent->GetVoxelizationParameters();
}

nvrhi::ITexture* AllocationMap::GetTexture() { return m_AllocationMap; }

Status AllocationMap::AllocateResources() {

    Status rc;

    auto params = GetVoxelizationParameters();

    // Binding layout
    {
        nvrhi::BindingLayoutDesc bindingDesc;
        bindingDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),  // AllocationMapCB
            nvrhi::BindingLayoutItem::Texture_UAV(0),             // u_AllocationMap
            nvrhi::BindingLayoutItem::Texture_UAV(1),             // u_InvalidateBitmap
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(2),         // u_OpacityToDownsample
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(3),         // u_OpacityToVoxelize
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(4),         // u_EmittanceToDownsample
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(5),         // u_EmissiveToVoxelize
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(6),         // u_IrradianceToTrace
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(7),         // u_OpacityToClear
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(8),         // u_OpacityToRestore
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(9),         // u_EmittanceToClear
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(10),        // u_DestPages
            nvrhi::BindingLayoutItem::Texture_SRV(0),             // t_AllocationMap
            nvrhi::BindingLayoutItem::Texture_SRV(1),             // t_InvalidateBitmap
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),    // t_InvalidateRegions
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3)};   // t_InvalidateLightFrusta
        bindingDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel |
                                 nvrhi::ShaderType::Compute;
        m_BindingLayout = m_Device->createBindingLayout(bindingDesc);
        if (!m_BindingLayout)
            return Status::RESOURCE_CREATION_FAILED;

        bindingDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
        bindingDesc.bindings = {
            nvrhi::BindingLayoutItem::PushConstants(0, 3 * sizeof(float)),
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(1),
            nvrhi::BindingLayoutItem::Texture_SRV(0)
        };
        m_DebugBindingLayout = m_Device->createBindingLayout(bindingDesc);
        if(!m_DebugBindingLayout)
            return Status::RESOURCE_CREATION_FAILED;
    }

    nvrhi::ShaderHandle cs, vs, ps;
    nvrhi::ComputePipelineDesc csoDesc;
    nvrhi::GraphicsPipelineDesc psoDesc;

    csoDesc.bindingLayouts = {m_BindingLayout};

    cs = m_ShaderFactory->CreateShader("app/AllocationMap/DilateEmissivePagesCS.hlsl",
                                       "main", nullptr, nvrhi::ShaderType::Compute);
    if (!cs)
        return Status::RESOURCE_CREATION_FAILED;
    csoDesc.CS = cs;
    m_DilateEmissivePagesCS = m_Device->createComputePipeline(csoDesc);
    if (!m_DilateEmissivePagesCS)
        return Status::RESOURCE_CREATION_FAILED;

    m_VoxelizeDebugPS = m_ShaderFactory->CreateShader("app/AllocationMap/DebugAllocationMapPS.hlsl",
                                       "main", nullptr, nvrhi::ShaderType::Pixel);
    if (!m_VoxelizeDebugPS)
        return Status::RESOURCE_CREATION_FAILED;

    std::vector<donut::engine::ShaderMacro> invalidateMacros;
    const char *persistentVoxelDataValue;
    if (params->persistentVoxelData) {
        if (params->simplifiedInvalidate)
            persistentVoxelDataValue = "2";
        else
            persistentVoxelDataValue = "1";
    } else
        persistentVoxelDataValue = "0";

    invalidateMacros.emplace_back("PERSISTENT_VOXEL_DATA", persistentVoxelDataValue);
    cs = m_ShaderFactory->CreateShader("app/AllocationMap/InvalidateRegionsCS", "main",
                                       &invalidateMacros, nvrhi::ShaderType::Compute);
    if (!cs)
        return Status::RESOURCE_CREATION_FAILED;
    csoDesc.CS = cs;
    m_InvalidateRegionCS = m_Device->createComputePipeline(csoDesc);
    if (!m_InvalidateRegionCS)
        return Status::RESOURCE_CREATION_FAILED;

    cs = m_ShaderFactory->CreateShader("app/AllocationMap/GeneratePageListsCS", "main",
                                       nullptr, nvrhi::ShaderType::Compute);
    if (!cs)
        return Status::RESOURCE_CREATION_FAILED;
    csoDesc.CS = cs;
    m_GeneratePageListsCS = m_Device->createComputePipeline(csoDesc);
    if (!m_GeneratePageListsCS)
        return Status::RESOURCE_CREATION_FAILED;

    cs = m_ShaderFactory->CreateShader(
        "app/AllocationMap/ComputeDispatchArgumentsFilteredCS", "main", nullptr,
        nvrhi::ShaderType::Compute);
    if (!cs)
        return Status::RESOURCE_CREATION_FAILED;
    csoDesc.CS = cs;
    m_ComputeDispatchArgumentsFilteredCS = m_Device->createComputePipeline(csoDesc);
    if (!m_ComputeDispatchArgumentsFilteredCS)
        return Status::RESOURCE_CREATION_FAILED;

    cs = m_ShaderFactory->CreateShader(
        "app/AllocationMap/ComputeIrradianceDispatchArgumentsCS", "main", nullptr,
        nvrhi::ShaderType::Compute);
    if (!cs)
        return Status::RESOURCE_CREATION_FAILED;
    csoDesc.CS = cs;
    m_ComputeIrradianceDispatchArgumentsCS = m_Device->createComputePipeline(csoDesc);
    if (!m_ComputeIrradianceDispatchArgumentsCS)
        return Status::RESOURCE_CREATION_FAILED;

    if (params->persistentVoxelData && params->simplifiedInvalidate) {
        vs =
            m_ShaderFactory->CreateShader("app/AllocationMap/RasterizeInvalidatedRegionsVS",
                                          "main", nullptr, nvrhi::ShaderType::Vertex);
        if (!vs)
            return Status::RESOURCE_CREATION_FAILED;
        ps =
            m_ShaderFactory->CreateShader("app/AllocationMap/RasterizeInvalidatedRegionsPS",
                                          "main", nullptr, nvrhi::ShaderType::Pixel);
        if (!ps)
            return Status::RESOURCE_CREATION_FAILED;

        nvrhi::VertexAttributeDesc inputDescs[] = {
            nvrhi::VertexAttributeDesc{}
                .setName("XY_BOUNDS")
                .setFormat(nvrhi::Format::RGBA32_FLOAT)
                .setElementStride(sizeof(RegionToRasterize))
                .setIsInstanced(true),
            nvrhi::VertexAttributeDesc{}
                .setName("Z_BITS")
                .setFormat(nvrhi::Format::RGBA32_UINT)
                .setOffset(offsetof(RegionToRasterize, zBits))
                .setElementStride(sizeof(RegionToRasterize))
                .setIsInstanced(true)};

        auto inputLayout = m_Device->createInputLayout(inputDescs, 2, vs);

        psoDesc = {};
        psoDesc.bindingLayouts = {m_BindingLayout};
        psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        psoDesc.inputLayout = inputLayout;
        psoDesc.VS = vs;
        psoDesc.PS = ps;
        psoDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        psoDesc.renderState.rasterState.scissorEnable = false;
        // No color buffer and depth buffer, only rasterize.
        m_RasterizeInvalidateRegionsPS = m_Device->createGraphicsPipeline(psoDesc, nvrhi::FramebufferInfo{});
    }

    {
        nvrhi::TextureDesc texDesc;
        texDesc.dimension = nvrhi::TextureDimension::Texture3D;
        texDesc.format = nvrhi::Format::R32_UINT;
        texDesc.width = params->allocationMapSize;
        texDesc.height = params->allocationMapSize;
        texDesc.depth = params->allocationMapSize;
        texDesc.isTypeless = true;
        texDesc.isUAV = 1;
        texDesc.keepInitialState = true;
        texDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        texDesc.debugName = "AllocationMap:AllocationMap";
        m_AllocationMap = m_Device->createTexture(texDesc);
        if (!m_AllocationMap)
            return Status::RESOURCE_CREATION_FAILED;
    }

    {
        nvrhi::BufferDesc bufDesc;
        bufDesc.byteSize = sizeof(RaycastCB);
        bufDesc.isConstantBuffer = true;
        bufDesc.isVolatile = true;
        bufDesc.maxVersions = m_Parent->GetNumFramesInFlight();
        bufDesc.debugName = "AllocationMap:RaycastCB";
        m_DebugBuffer = m_Device->createBuffer(bufDesc);
    }

    {
        nvrhi::BufferDesc bufDesc;
        bufDesc.byteSize = sizeof(InvalidateCB);
        bufDesc.isConstantBuffer = true;
        bufDesc.isVolatile = true;
        bufDesc.maxVersions = m_Parent->GetNumFramesInFlight();
        bufDesc.debugName = "AllocationMap:AllocationMapCB";
        m_InvalidateCB = m_Device->createBuffer(bufDesc);
    }

    {
        nvrhi::BufferDesc bufDesc;

        // Both buffers are bound as SRVs on every InvalidateRegions dispatch, but they
        // are only written when there is something to invalidate that frame. On a frame
        // with no invalidated regions or light frusta they reach the binding set without
        // nvrhi having ever seen a state transition for them, which trips "Unknown prior
        // state of buffer ...". Declaring the initial state makes them valid to bind
        // before the first write, the same way the invalidate bitmap textures below do.
        bufDesc.keepInitialState = true;
        bufDesc.initialState = nvrhi::ResourceStates::ShaderResource;

        if(!params->simplifiedInvalidate) {
            bufDesc.byteSize = sizeof(VxgiBox4i) * MAX_INVALIDATE_REGIONS;
            bufDesc.structStride = sizeof(VxgiBox4i);
            bufDesc.debugName = "AllocationMap:InvalidateRegions";
            m_InvalidateBuffer = m_Device->createBuffer(bufDesc);
        }

        bufDesc.byteSize = sizeof(Frustum) * MAX_INVALIDATE_REGIONS;
        bufDesc.structStride = sizeof(Frustum);
        bufDesc.debugName = "AllocationMap::InvalidateLightFrusta";
        m_InvalidateFrustaBuffer = m_Device->createBuffer(bufDesc);
    }

    if (params->persistentVoxelData && params->simplifiedInvalidate) {
        m_InvalidateBufferSize = 0;
        nvrhi::TextureDesc bitmapDesc;
        bitmapDesc.dimension = nvrhi::TextureDimension::Texture3D;
        bitmapDesc.format = nvrhi::Format::R32_UINT;
        bitmapDesc.width = params->allocationMapSize;
        bitmapDesc.height = params->allocationMapSize;
        bitmapDesc.depth = params->allocationMapSize;
        bitmapDesc.isUAV = 1;
        bitmapDesc.keepInitialState = true;
        bitmapDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        bitmapDesc.debugName = "AllocationMap:InvalidateBitmap";
        m_InvalidateBitmap = m_Device->createTexture(bitmapDesc);
    } else {
        nvrhi::TextureDesc bitmapDesc;
        bitmapDesc.dimension = nvrhi::TextureDimension::Texture3D;
        bitmapDesc.format = nvrhi::Format::R32_UINT;
        bitmapDesc.width = 1;
        bitmapDesc.height = 1;
        bitmapDesc.depth = 1;
        bitmapDesc.isUAV = 1;
        bitmapDesc.keepInitialState = true;
        bitmapDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        bitmapDesc.debugName = "AllocationMap:InvalidateBitmap(Unused)";
        m_InvalidateBitmap = m_Device->createTexture(bitmapDesc);
    }

    std::vector<uint> pagesPerLevel((size_t)params->stackLevels);
    for (uint level = 0; level < params->stackLevels; ++level) {
        uint logSize = params->stackLevels - level - 1;
        uint pagesNum = (params->allocationMapSize >> logSize) *
                        (params->allocationMapSize >> logSize) *
                        (params->allocationMapSize >> logSize);
        pagesPerLevel[level] = pagesNum;
    }

    uint defaultLogPageSize0 = params->allocationMapLod;
    rc = createMultiPageList(pagesPerLevel, defaultLogPageSize0, &m_OpacityPagesToRestore);
    if (rc != Status::OK)
        return rc;

    rc = createMultiPageList(pagesPerLevel, defaultLogPageSize0, &m_OpacityPagesToVoxelize);
    if (rc != Status::OK)
        return rc;

    rc = createMultiPageList(pagesPerLevel, defaultLogPageSize0,
                             &m_PagesWithEmissiveMaterialsToVoxelize);
    if (rc != Status::OK)
        return rc;

    pagesPerLevel.resize(params->pageLevels);
    for (uint level = params->stackLevels; level < params->pageLevels; ++level) {
        uint pagesNum = params->allocationMapSize * params->allocationMapSize *
                        params->allocationMapSize;
        pagesPerLevel[level] = pagesNum;
    }

    rc = createMultiPageList(pagesPerLevel, defaultLogPageSize0, &m_OpacityPagesToClear);
    if (rc != Status::OK)
        return rc;

    rc = createMultiPageList(pagesPerLevel, defaultLogPageSize0,
                             &m_OpacityPagesToDownsample);
    if (rc != Status::OK)
        return rc;

    rc = createMultiPageList(pagesPerLevel, defaultLogPageSize0,
                             &m_EmittancePagesToClear);
    if (rc != Status::OK)
        return rc;

    rc = createMultiPageList(pagesPerLevel, defaultLogPageSize0,
                             &m_EmittancePagesToDownsample);
    if (rc != Status::OK)
        return rc;

    uint logIrradiancePageSize =
        std::max<int>((int)params->allocationMapLodBias - params->indirectIrradianceMapLodBias, 0);

    pagesPerLevel[0] = pagesPerLevel[(int)pagesPerLevel.size() - 1];
    pagesPerLevel.resize(1);
    rc = createMultiPageList(pagesPerLevel, logIrradiancePageSize,
                             &m_IrradiancePagesToProcess[0]);
    if (rc != Status::OK)
        return rc;

    rc = createMultiPageList(pagesPerLevel, logIrradiancePageSize,
                             &m_IrradiancePagesToProcess[1]);
    if (rc != Status::OK)
        return rc;

    // Binding sets
    {
        m_BindingCache->Clear();

        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::Texture_UAV(1, m_InvalidateBitmap)};
        m_RasterizeInvalidateRegionsBindingSet = m_Device->createBindingSet(bindingSetDesc, m_BindingLayout);

        m_InvalidateRegionsBindingSet = nullptr;

        bindingSetDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_InvalidateCB),
                                   nvrhi::BindingSetItem::Texture_UAV(0, m_AllocationMap)};
        m_DilateEmissivePagesBindingSet =
            m_Device->createBindingSet(bindingSetDesc, m_BindingLayout);

        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_InvalidateCB),
            nvrhi::BindingSetItem::Texture_SRV(0, m_AllocationMap),
            nvrhi::BindingSetItem::TypedBuffer_UAV(2, m_OpacityPagesToDownsample.buffer),
            nvrhi::BindingSetItem::TypedBuffer_UAV(3, m_OpacityPagesToVoxelize.buffer),
            nvrhi::BindingSetItem::TypedBuffer_UAV(4, m_EmittancePagesToDownsample.buffer),
            nvrhi::BindingSetItem::TypedBuffer_UAV(5, m_PagesWithEmissiveMaterialsToVoxelize.buffer),
            nvrhi::BindingSetItem::None()
        };

        for (int i = 0; i < 2; ++i) {
            bindingSetDesc.bindings[6] = nvrhi::BindingSetItem::TypedBuffer_UAV(
                6, m_IrradiancePagesToProcess[i].buffer);
            m_GeneratePageListsBindingSets[i] =
                m_Device->createBindingSet(bindingSetDesc, m_BindingLayout);
        }
    }

    {
        nvrhi::FramebufferDesc fbDesc;
        m_EmptyFramebuffer = m_Device->createFramebuffer(fbDesc);
    }

    return rc;
}

Status AllocationMap::CreatePageProcessingCS(const char* sourceFile, bool allPagedLevels,
                                             const donut::engine::ShaderMacro* macros,
                                             uint numMacros,
                                             std::vector<nvrhi::ShaderHandle>& shaders) {
    std::vector<donut::engine::ShaderMacro> macrosWithSize{
        { "GROUP_SIZE", "0" }
    };
    if(numMacros)
        macrosWithSize.insert(macrosWithSize.end(), macros, macros + numMacros);

    auto params = GetVoxelizationParameters();
    uint pageLevels = allPagedLevels ? params->pageLevels : params->stackLevels;
    uint previousGroupSize = 0;

    shaders.resize(pageLevels);
    for (int level = pageLevels - 1; level >= 0; --level) {
        uint groupSize = GetPageProcessingGroupSize(level);
        if(groupSize == previousGroupSize)  {
            shaders[level] = shaders[level + 1];
        } else {
            previousGroupSize = groupSize;
            macrosWithSize[0].definition = std::to_string(groupSize);

            auto cs = m_ShaderFactory->CreateShader(sourceFile, "main", &macrosWithSize,
                                                    nvrhi::ShaderType::Compute);
            if(!cs)
                return Status::RESOURCE_CREATION_FAILED;

            shaders[level] = cs;
        }
    }

    return Status::OK;
}

void AllocationMap::SnapAndInvalidateRegions(nvrhi::ICommandList* comandList, const std::vector<box3>& regions,
                                             const std::vector<frustum>& frusta,
                                             std::vector<box3>& snappedRegions, bool anchorMoved) {

    auto Params = GetVoxelizationParameters();
    auto ClipmapGeometry = m_Parent->GetClipmapGeometry();
    auto CurrentPerGPUData = m_Parent->GetCurrentPerGPUData();

    if (!CurrentPerGPUData->m_AllocationCleared) {
        comandList->clearTextureUInt(m_AllocationMap, nvrhi::AllSubresources, 0u);
        CurrentPerGPUData->m_AllocationCleared = 1;
        CurrentPerGPUData->m_ToroidalOffsetPreviousFrame = ClipmapGeometry->m_AllocationMapToroidalOffset;
    }

    uint numInvalidateRegions = 0;
    uint numInvalidateFrustas = 0;

    if (Params->persistentVoxelData) {
        if (Params->simplifiedInvalidate) {
            comandList->clearTextureUInt(m_InvalidateBitmap, nvrhi::AllSubresources, 0u);

            if (!regions.empty()) {
                const uint numRegions = regions.size();
                std::vector<RegionToRasterize> rasterRegions((size_t)numRegions);
                box3 rasterRegion = box3::empty();
                const float b = 1.f / ClipmapGeometry->m_PageSize;
                const float PixelSize = 1.f / float(Params->allocationMapSize);
                uint idx = 0;
                for (uint j = 0; j < numRegions; ++j) {
                    auto& region = regions[j];
                    box3 intersect = region & (ClipmapGeometry->m_WorldRegion);
                    if (all(intersect.lower() < intersect.upper())){
                        box3 other;
                        other.lower() = vfloor(
                            (intersect.lower() - ClipmapGeometry->m_WorldRegion.lower()) *
                            b);
                        other.upper() = vceil(
                            (intersect.upper() - ClipmapGeometry->m_WorldRegion.lower()) *
                            b);

                        rasterRegion |= other;

                        auto& rasterRegion = rasterRegions[idx++];
                        rasterRegion.xyBounds.x = other.lower().x * PixelSize * 2.f - 1.f;
                        rasterRegion.xyBounds.x = other.lower().y * PixelSize * 2.f - 1.f;
                        rasterRegion.xyBounds.z = other.upper().x * PixelSize * 2.f - 1.f;
                        rasterRegion.xyBounds.w = other.upper().y * PixelSize * 2.f - 1.f;

                        for (uint i = 0; i < 4; ++i) {
                            int left = std::max<int>((int)other.lower().z - 32 * i, 0);
                            int right = std::max<int>(32 * i + 32 - (int)other.lower().z, 0);
                            if (left < 32 && right < 32) {
                                rasterRegion.zBits[i] = (0xFFFFFFFF >> right) & (-1 << left);
                            }
                        }
                    }
                }

                if (idx > 0) {
                    rasterRegion.lower() -= 0.5f;
                    rasterRegion.upper() += 0.5f;
                    rasterRegion.lower() *= ClipmapGeometry->m_PageSize;
                    rasterRegion.upper() *= ClipmapGeometry->m_PageSize;
                    rasterRegion.lower() += ClipmapGeometry->m_WorldRegion.lower();
                    rasterRegion.upper() += ClipmapGeometry->m_WorldRegion.lower();

                    snappedRegions.push_back(rasterRegion);

                    if (m_InvalidateBufferSize < numRegions) {
                        m_InvalidateBufferSize = 1 << (int)std::ceil(std::log2<uint>(numRegions));
                        m_InvalidateBufferSize =
                            std::max<int>(MAX_INVALIDATE_REGIONS, m_InvalidateBufferSize);

                        nvrhi::BufferDesc bufDesc;
                        bufDesc.byteSize =
                            sizeof(RegionToRasterize) * m_InvalidateBufferSize;
                        bufDesc.isVertexBuffer = true;
                        bufDesc.cpuAccess = nvrhi::CpuAccessMode::Write;
                        bufDesc.debugName = "AllocationMap::InvalidateRegions";
                        m_InvalidateBuffer = m_Device->createBuffer(bufDesc);
                        m_InvalidateRegionsBindingSet = nullptr; // require recreate
                    }

                    comandList->writeBuffer(m_InvalidateBuffer, rasterRegions.data(),
                                            idx * sizeof(RegionToRasterize));

                    nvrhi::GraphicsState state;
                    state.pipeline = m_RasterizeInvalidateRegionsPS;
                    state.bindings = { m_RasterizeInvalidateRegionsBindingSet };
                    state.viewport.addViewport(
                        {(float)Params->allocationMapSize, (float)Params->allocationMapSize});
                    state.framebuffer = m_EmptyFramebuffer;
                    comandList->setGraphicsState(state);

                    state.vertexBuffers = { nvrhi::VertexBufferBinding{ m_InvalidateBuffer } };

                    nvrhi::DrawArguments args;
                    args.vertexCount = 4;
                    args.instanceCount = idx;
                    comandList->drawIndexed(args);
                }
            }
        } else {
            float3 pageSizef = ClipmapGeometry->m_PageSize;

            m_RegionsInVoxel.resize(0);
            for(auto &region : regions) {
                if(region.intersects(ClipmapGeometry->m_WorldRegion)) {
                    auto intersect = region & ClipmapGeometry->m_WorldRegion;
                    ibox3 regionInVoxel;
                    regionInVoxel.lower() = int3(vfloor(intersect.lower() / pageSizef));
                    regionInVoxel.upper() = int3(vceil(intersect.upper() / pageSizef));
                    m_RegionsInVoxel.push_back(regionInVoxel);
                }
            }

            auto inRegion = &m_RegionsInVoxel;
            auto outRegion = &m_RegionsInVoxel2;

            OptimizeRegion(inRegion, outRegion);
            while(1) {
                uint numOutRegion = outRegion->size();
                uint numInRegion = inRegion->size();
                if(numOutRegion >= numInRegion)
                    break;

                std::swap(inRegion, outRegion);
                OptimizeRegion(inRegion, outRegion);
            }

            int3 pageOrigin = int3(ClipmapGeometry->m_WorldRegion.lower() / pageSizef);
            /* box<int, 4> */ VxgiBox4i invalidateRegions[MAX_INVALIDATE_REGIONS];
            for (auto it = outRegion->begin(); it != outRegion->end() && numInvalidateRegions < (uint)MAX_INVALIDATE_REGIONS; ++it) {
                box3 snappedRegion;
                snappedRegion.lower() = (float3(it->lower()) - 0.5f) * pageSizef;
                snappedRegion.upper() = (float3(it->upper()) + 0.5f) * pageSizef;
                snappedRegions.push_back(snappedRegion);

                VxgiBox4i invalidateRegion;
                invalidateRegion.lower = int4(it->lower() - pageOrigin, 0);
                invalidateRegion.upper = int4(it->upper() - 1 - pageOrigin, 0);
                invalidateRegions[numInvalidateRegions++] = invalidateRegion;
            }

            comandList->writeBuffer(m_InvalidateBuffer, invalidateRegions,
                                    sizeof(invalidateRegions));
        }

        numInvalidateFrustas = std::min<uint>(frusta.size(), MAX_INVALIDATE_REGIONS);
        if(numInvalidateFrustas) {
            Frustum invalidateFrusta[MAX_INVALIDATE_REGIONS];
            for (uint k = 0; k < numInvalidateFrustas; ++k)
                invalidateFrusta[k] = (const Frustum&)(frusta[k]);
            comandList->writeBuffer(m_InvalidateFrustaBuffer, invalidateFrusta,
                                    sizeof(invalidateFrusta));
        }
    } else {
        snappedRegions.push_back(ClipmapGeometry->m_WorldRegion);
    }

    InvalidateCB cb;
    memset(&cb, 0, sizeof(cb));
    for (uint level = 0; level < Params->pageLevels; ++level) {
        cb.filteredPageListParams[level] = m_OpacityPagesToDownsample.getListParams(level);

        uint PageProcessingGroupSize = GetPageProcessingGroupSize(level);

        if(PageProcessingGroupSize == 1) {
            cb.gridParams[level] = uint4(0x3F, 6u, 0, 0);
        } else {
            if(PageProcessingGroupSize == 2)
                cb.gridParams[level] = uint4(7u, 3u, 0, 0);
            else {
                cb.gridParams[level] = uint4(
                    0, 0,
                    Params->allocationMapLod - level - log2<uint>(PageProcessingGroupSize), 0);
            }
        }
    }

    cb.irradianceMapPageListParams = m_IrradiancePagesToProcess->getListParams(0);
    cb.irradianceMapGridParams =
        cb.gridParams[std::max<int>(0, Params->indirectIrradianceMapLod)];
    cb.textureSize = Params->allocationMapSize;
    cb.toroidalOffset = int4(ClipmapGeometry->m_AllocationMapToroidalOffset, 0);
    cb.toroidalOffsetPrevious = int4(CurrentPerGPUData->m_ToroidalOffsetPreviousFrame, 0);
    cb.clipmapOrigin = float4(ClipmapGeometry->m_WorldRegion.lower(), ClipmapGeometry->m_PageSize);
    cb.numberOfRegions = numInvalidateRegions;
    cb.maxClipLevel = Params->stackLevels - 1;
    cb.numberOfFrusta = numInvalidateFrustas;
    cb.numPagedLevels = Params->pageLevels;
    cb.opacityVoxelizeAllLevels = 1;
    cb.opacityDownsampleAllPresentPages = anchorMoved;
    cb.storeEmittanceInFP16 = Params->emittanceFormat == EmittanceFormat::FLOAT16;
    cb.emittanceVoxelizeAllLevels = Params->useEmittanceInterpolation;
    cb.writeIrradianceMapPages = Params->indirectIrradianceMapLod > 0;
    comandList->writeBuffer(m_InvalidateCB, &cb, sizeof(cb));

    if(!m_InvalidateRegionsBindingSet) {
        nvrhi::BindingSetDesc bindingSetDesc;

        bindingSetDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_InvalidateCB),
                                   nvrhi::BindingSetItem::Texture_UAV(0, m_AllocationMap),
                                   nvrhi::BindingSetItem::TypedBuffer_UAV(7, m_OpacityPagesToClear.buffer),
                                   nvrhi::BindingSetItem::TypedBuffer_UAV(8, m_OpacityPagesToRestore.buffer),
                                   nvrhi::BindingSetItem::TypedBuffer_UAV(9, m_EmittancePagesToClear.buffer)};

        if(Params->persistentVoxelData) {
            if(Params->simplifiedInvalidate)
                bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(1, m_InvalidateBitmap));
            else
                bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_InvalidateBuffer));

            bindingSetDesc.bindings.push_back(
                nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_InvalidateFrustaBuffer));
        }

        m_InvalidateRegionsBindingSet =
            m_Device->createBindingSet(bindingSetDesc, m_BindingLayout);
    }

    nvrhi::ComputeState state;
    state.pipeline = m_InvalidateRegionCS;
    state.bindings = { m_InvalidateRegionsBindingSet };
    comandList->setComputeState(state);

    clearMultiPageList(comandList, &m_OpacityPagesToClear);
    clearMultiPageList(comandList, &m_OpacityPagesToRestore);
    clearMultiPageList(comandList, &m_EmittancePagesToClear);

    constexpr uint INVALIDATE_GROUP_SIZE = 4;
    uint3 numGroups = div_ceil(Params->allocationMapSize, INVALIDATE_GROUP_SIZE);
    comandList->dispatch(numGroups.x, numGroups.y, numGroups.z);

    ComputeDispatchArguments(comandList, &m_OpacityPagesToClear, false);
    ComputeDispatchArguments(comandList, &m_OpacityPagesToRestore, false);
    ComputeDispatchArguments(comandList, &m_EmittancePagesToClear, false);
}

void AllocationMap::RenderDebug(nvrhi::ICommandList* commandList, nvrhi::GraphicsState& state,
                                const float4x4& viewProjMatrix, float nearClipZ, float farClipZ,
                                const float3& cameraPos, uint bitToDisplay, uint voxelToSkip,
                                float targetOpacity) {
    auto params = GetVoxelizationParameters();
    auto geometry = m_Parent->GetClipmapGeometry();
    auto& viewport = state.viewport.viewports[0];
    RaycastCB debugBuffer = {};
    debugBuffer.cameraPos = float4(cameraPos, 0.f);
    debugBuffer.gridBounds = float4(geometry->m_WorldRegion.lower(), geometry->m_WorldSize);
    debugBuffer.viewProjMatrix = viewProjMatrix;
    debugBuffer.viewProjMatrixInv = inverse(viewProjMatrix);
    debugBuffer.mipOffset = 0;
    debugBuffer.textureSize = params->allocationMapSize;
    debugBuffer.opacityPackingOffset = 0;
    debugBuffer.emittancePackingOffset = 0;
    debugBuffer.toroidalOffset = int4(geometry->m_AllocationMapToroidalOffsetWrapped, 0);
    debugBuffer.voxelsToSkip = voxelToSkip;
    debugBuffer.skipUpperLevels = 0;
    debugBuffer.farClipZ = farClipZ;
    debugBuffer.viewportMinZ = viewport.minZ;
    debugBuffer.viewportMaxZ = viewport.maxZ;
    debugBuffer.allocationMapBit = bitToDisplay;
    debugBuffer.targetOpacity = targetOpacity;

    commandList->writeBuffer(m_DebugBuffer, &debugBuffer, sizeof(debugBuffer));

    if(m_VoxelizeDebugPSO) {
        if(m_VoxelizeDebugPSO->getFramebufferInfo() != state.framebuffer->getFramebufferInfo())
            m_VoxelizeDebugPSO = nullptr;
    }

    if(!m_VoxelizeDebugPSO) {
        nvrhi::GraphicsPipelineDesc psoDesc;
        psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        psoDesc.bindingLayouts = {m_DebugBindingLayout};
        psoDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        psoDesc.renderState.rasterState.depthClipEnable = 0;
        psoDesc.VS = m_Parent->GetFullScreenQuadVS();
        psoDesc.PS = m_VoxelizeDebugPS;
        psoDesc.renderState.blendState.targets[0]
            .setBlendEnable(true)
            .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
            .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha);

        m_VoxelizeDebugPSO = m_Device->createGraphicsPipeline(psoDesc, state.framebuffer->getFramebufferInfo());
        NVRHI_ASSERT(m_VoxelizeDebugPSO);
    }

    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {
        nvrhi::BindingSetItem::PushConstants(0, 3 * sizeof(float)),
        nvrhi::BindingSetItem::ConstantBuffer(1, m_DebugBuffer),
        nvrhi::BindingSetItem::Texture_SRV(0, m_AllocationMap),
    };
    state.pipeline = m_VoxelizeDebugPSO;
    state.bindings = { m_BindingCache->GetOrCreateBindingSet(bindingSetDesc, m_DebugBindingLayout) };
    commandList->setGraphicsState(state);

    float fullscreenCB[] = { nearClipZ, farClipZ, nearClipZ };
    commandList->setPushConstants(fullscreenCB, sizeof(fullscreenCB));
    nvrhi::DrawArguments args;
    args.vertexCount = 4;
    commandList->draw(args);
}

void AllocationMap::SetPerGpuAlternatingFrameIndex(bool index) {
    m_AlternatingFrameIndex = index;

}

nvrhi::ITexture* AllocationMap::GetInvalidateBitmap() { return m_InvalidateBitmap; }

void AllocationMap::OptimizeRegion(const std::vector<ibox3>* inRegion,
                                   std::vector<ibox3>* outRegion) {
    uint numRegion = (uint)inRegion->size();
    outRegion->clear();
    m_RegionVolumes.resize(numRegion);
    for (uint i = 0; i < numRegion; ++i) {
        auto& region = inRegion->at(i);
        auto d = dm::max(region.upper() - region.lower(), int3::zero());
        m_RegionVolumes[i] = d.x * d.y * d.z;
    }

    m_RegionGains.resize(numRegion * (numRegion - 1) / 2);
    std::vector<bool> mergeFlags((size_t)numRegion, false);

    uint pairIndex = 0;

    for (uint i = 0; i < numRegion; ++i) {

        if(mergeFlags[i])
            continue;

        const auto& region1 = inRegion->at(i);
        int vol1 = m_RegionVolumes[i];
        int maxGain = -1;
        int closestRegion = 0;

        ibox3 OptimalUnification = ibox3::empty();

        for (uint j = i + 1; j < numRegion; ++j) {

            if(mergeFlags[j])
                continue;

            const auto& region2 = inRegion->at(j);
            int vol2 = m_RegionVolumes[j];
            ibox3 UnifiedRegion = region1 | region2;
            auto d = dm::max(UnifiedRegion.upper() - UnifiedRegion.lower(), int3::zero());
            int UnifiedRegionVol = d.x * d.y * d.z;

            int gain = vol1 + vol2 - UnifiedRegionVol;
            m_RegionGains[pairIndex++] = gain;

            if(maxGain < gain) {
                maxGain = gain;
                closestRegion = j;
                OptimalUnification = UnifiedRegion;
            }
        }

        if(maxGain > 0) {
            outRegion->push_back(OptimalUnification);
            mergeFlags[i] = true;
            mergeFlags[closestRegion] = true;
        }
    }

    for (uint i = 0; i < numRegion; ++i) {
        if(!mergeFlags[i]) {
            outRegion->push_back(inRegion->at(i));
        }
    }
}

uint AllocationMap::GetPageProcessingGroupSize(uint level) {
    auto params = GetVoxelizationParameters();
    uint pageSize = 1 << (params->pageLevels - 1 - level);
    return std::min<uint>(pageSize, 4);
}

Status AllocationMap::createMultiPageList(const std::vector<uint>& pageCounts,
                                          uint logPageSize0, MultiPageList* list) {
    list->buffer = nullptr;
    list->pageCounts = pageCounts;
    list->logPageSize0 = logPageSize0;

    uint numLevels = (uint)pageCounts.size();
    list->offsets.resize(numLevels);

    uint offset = 0;
    for (uint level = 0; level < numLevels; ++level) {
        list->offsets[level] = offset;
        offset += pageCounts[level] + 4;
    }

    list->bufferSize = 4 * offset;

    nvrhi::BufferDesc desc;
    desc.byteSize = list->bufferSize;
    desc.canHaveTypedViews = true;
    desc.canHaveUAVs = 1;
    desc.isDrawIndirectArgs = 1;
    desc.format = nvrhi::Format::R32_UINT;
    desc.keepInitialState = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    list->buffer = m_Device->createBuffer(desc);
    if (!list->buffer)
        return Status::RESOURCE_CREATION_FAILED;

    return Status::OK;
}

void AllocationMap::clearMultiPageList(nvrhi::ICommandList *commandList, MultiPageList* list) {
    uint counterOffset = 0;
    commandList->clearBufferUInt(list->buffer, 0);
}

void AllocationMap::ComputeDispatchArguments(nvrhi::ICommandList* commandList,
                                             MultiPageList* list, bool irradianceMap) {
    uint NumLevels = list->getNumLevels();

    nvrhi::ComputeState state;
    if(irradianceMap)
        state.pipeline = m_ComputeIrradianceDispatchArgumentsCS;
    else
        state.pipeline = m_ComputeDispatchArgumentsFilteredCS;

    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_InvalidateCB),
        nvrhi::BindingSetItem::TypedBuffer_UAV(10, list->buffer)
    };
    state.bindings = { m_BindingCache->GetOrCreateBindingSet(bindingSetDesc, m_BindingLayout) };
    commandList->setComputeState(state);
    commandList->dispatch(NumLevels, 1, 1);
}

nvrhi::IBindingSet* AllocationMap::GetIrradiancePageListBindingSet(bool previousFrame) {
    int alternatingFrameIndex;
    if(previousFrame)
        alternatingFrameIndex = !m_AlternatingFrameIndex;
    else
        alternatingFrameIndex = m_AlternatingFrameIndex;
    alternatingFrameIndex = !!alternatingFrameIndex;
    return m_GeneratePageListsBindingSets[alternatingFrameIndex];
}

MultiPageList* AllocationMap::GetIrradiancePageList(bool previousFrame) {
    int alternatingFrameIndex;
    if (previousFrame)
        alternatingFrameIndex = !m_AlternatingFrameIndex;
    else
        alternatingFrameIndex = m_AlternatingFrameIndex;
    alternatingFrameIndex = !!alternatingFrameIndex;
    return &m_IrradiancePagesToProcess[alternatingFrameIndex];
}

void AllocationMap::DilateEmissivePages(nvrhi::ICommandList* commandList) {
    auto Params = m_Parent->GetVoxelizationParameters();
    constexpr uint DILATE_EMISSIVE_PAGES_GROUP_SIZE = 8;
    uint3 numGroups = div_ceil(Params->allocationMapSize, DILATE_EMISSIVE_PAGES_GROUP_SIZE);

    nvrhi::ComputeState state;
    state.pipeline = m_DilateEmissivePagesCS;
    state.bindings = { m_DilateEmissivePagesBindingSet };
    commandList->setComputeState(state);
    commandList->dispatch(numGroups.x, numGroups.y, numGroups.z);
}

void AllocationMap::GeneratePageLists(nvrhi::ICommandList* commandList) {
    auto Params = GetVoxelizationParameters();

    clearMultiPageList(commandList, &m_OpacityPagesToDownsample);
    clearMultiPageList(commandList, &m_OpacityPagesToVoxelize);
    clearMultiPageList(commandList, &m_EmittancePagesToDownsample);
    clearMultiPageList(commandList, &m_PagesWithEmissiveMaterialsToVoxelize);
    clearMultiPageList(commandList, GetIrradiancePageList(false));

    nvrhi::ComputeState state;
    state.pipeline = m_GeneratePageListsCS;
    state.bindings = { GetIrradiancePageListBindingSet(0) };
    commandList->setComputeState(state);

    uint3 numGroups;
    numGroups.x = div_ceil(Params->allocationMapSize, 8u);
    numGroups.y = div_ceil(Params->allocationMapSize, 8u);
    numGroups.z = div_ceil(Params->allocationMapSize, 4u);
    commandList->dispatch(numGroups.x, numGroups.y, numGroups.z);

    ComputeDispatchArguments(commandList, &m_OpacityPagesToDownsample, 0);
    ComputeDispatchArguments(commandList, &m_OpacityPagesToVoxelize, 0);
    ComputeDispatchArguments(commandList, &m_EmittancePagesToDownsample, 0);
    ComputeDispatchArguments(commandList, &m_PagesWithEmissiveMaterialsToVoxelize, 0);
    ComputeDispatchArguments(commandList, GetIrradiancePageList(0), 1);
}

}  // namespace vxgi
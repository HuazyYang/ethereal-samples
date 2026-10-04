#include "meshlets/MeshletDrawStrategy.h"
#include "meshlets/MeshletRenderResources.h"
#include "app/GpuProfiler.h"
#include "meshlets/MeshletShaderTypes.h"
#include "meshlets/NvMeshShaderApi.h"
#include "scene/AsteroidLibrary.h"
#include "scene/SceneGraph.h"
#include "scene/SceneMaterial.h"
#include "scene/SpaceObject.h"
#include "scene/SpaceScene.h"
#include "scene/SpaceSector.h"

#include <donut/core/log.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/View.h>
#include <nvrhi/d3d12.h>
#include <nvrhi/utils.h>

#include <algorithm>
#include <climits>
#include <cstring>

using namespace donut;
using namespace donut::math;

namespace
{
    // Frame-global statistics of all strategies (Asteroids.exe: dword_1402DF260, qword_1402DF268, qword_1402DF270).
    uint32_t g_NumMeshletDraws = 0;
    uint64_t g_NumAsteroidInstances = 0;
    uint64_t g_NumAsteroidPrimitives = 0;

    constexpr uint32_t c_DebugReadbackEntries = 0x10000;   // x 20 bytes
    constexpr uint32_t c_StatsReadbackEntries = 16;        // x uint
    constexpr float c_InvZFarTileSize = 0.125f;            // create_hi_z_cs: 8x8 depth pixels per level-0 texel

    // cbFrame.blueNoise: 4x4 ordered-dither matrix ((bayer + 1) / 17, rounded to 3 digits) stored in
    // Asteroids.exe at 0x140256820 / 0x140256800 / 0x1402567F0.
    const float4x4 c_BlueNoise(
        0.059f, 0.529f, 0.176f, 0.647f,
        0.765f, 0.294f, 0.882f, 0.412f,
        0.235f, 0.706f, 0.118f, 0.588f,
        0.941f, 0.471f, 0.824f, 0.353f);

    // Material names drawn with the IS_SHIP pixel shader permutation (0x140042400).
    bool IsShipMaterialName(const std::string& name)
    {
        return name.compare(0, 13, "SF_Gargoship_") == 0 || name.compare(0, 8, "MatGlow_") == 0;
    }

    float3x4 ToShaderInstanceMatrix(const affine3& transform)
    {
        // Rows of mul(instanceMat, float4(p, 1)) for the row-major float3x4 of cbInstance.
        float3x4 result;
        affineToColumnMajor(transform, result.m_data);
        return result;
    }

    nvrhi::BufferHandle CreateReadbackBuffer(nvrhi::IDevice* device, uint64_t byteSize)
    {
        nvrhi::BufferDesc desc;
        desc.byteSize = byteSize;
        desc.debugName = "DebugUAVDest";
        desc.cpuAccess = nvrhi::CpuAccessMode::Read;
        desc.initialState = nvrhi::ResourceStates::CopyDest;
        desc.keepInitialState = true;
        return device->createBuffer(desc);
    }

    // 2018: 0x14000BE30 created the layout from the first binding set desc and stored it in the shared slot.
    nvrhi::BindingSetHandle CreateBindingSet(nvrhi::IDevice* device, const nvrhi::BindingSetDesc& setDesc,
        nvrhi::BindingLayoutHandle& layout)
    {
        constexpr uint32_t c_MeshletRegisterSpace = 1;
        if (!layout)
        {
            nvrhi::BindingSetHandle set;
            if (!nvrhi::utils::CreateBindingSetAndLayout(device, nvrhi::ShaderType::All, c_MeshletRegisterSpace, setDesc, layout, set))
                return nullptr;
            return set;
        }
        return device->createBindingSet(setDesc, layout);
    }
}

MeshletDrawStrategy::MeshletDrawStrategy(
    nvrhi::IDevice* device,
    std::shared_ptr<engine::ShaderFactory> shaderFactory,
    std::shared_ptr<SpaceScene> scene,
    std::shared_ptr<MeshletRenderResources> resources,
    nvrhi::ITexture* depthBuffer,
    nvrhi::ITexture* hiZBuffer,
    nvrhi::ITexture* randomsTexture,
    nvrhi::ITexture* viewDistanceTexture,
    const std::string& pixelShaderFile,
    MeshletRenderRole role)
    : m_Role(role)
    , m_Scene(std::move(scene))
    , m_Device(device)
    , m_ShaderFactory(std::move(shaderFactory))
    , m_Resources(std::move(resources))
    , m_PixelShaderFile(pixelShaderFile)
    , m_RandomsTexture(randomsTexture ? randomsTexture : m_Resources->nullTexture.Get())
    , m_ViewDistanceTexture(viewDistanceTexture ? viewDistanceTexture : m_Resources->nullTexture.Get())
    , m_HiZTexture(hiZBuffer ? hiZBuffer : m_Resources->nullTexture.Get())
{
    // Asteroids.exe: 0x1400400E0
    // The CPU copies start filled with 0xEE (memset -286331154 in 2018).
    m_DebugReadbackData.assign(size_t(c_DebugReadbackEntries) * 20, 0xEE);
    m_DebugReadbackBuffer = CreateReadbackBuffer(device, m_DebugReadbackData.size());
    m_StatsReadbackData.assign(c_StatsReadbackEntries, 0xEEEEEEEEu);
    m_StatsReadbackBuffer = CreateReadbackBuffer(device, c_StatsReadbackEntries * sizeof(uint32_t));

    CreateShaders();

    // deviation: the 2018 constructor also copied the scene's (non-meshlet) mesh instance list and sorted it by
    // buffers / material / mesh / instance index like InstancedOpaqueDrawStrategy (+48); nothing reads that list.

    if (m_Role == MeshletRenderRole::GBuffer)
        m_HiZPass = std::make_unique<HiZPass>(device, *m_ShaderFactory, depthBuffer, hiZBuffer);

    if (m_Role == MeshletRenderRole::AsteroidsOnly)
    {
        drawPlayerShip = false;
        drawOtherObjects = false;
    }
}

void MeshletDrawStrategy::CreateShaders()
{
    // Asteroids.exe: 0x1400412D0
    MeshletShaderSetDesc desc;
    desc.taskShader = "basicTS";
    desc.meshShader = "basicMS";
    desc.pixelShader = m_PixelShaderFile;
    desc.depthPrePass = (m_Role == MeshletRenderRole::Depth);
    m_BasicShaders = std::make_unique<MeshletShaderSet>(m_Device, *m_ShaderFactory, desc);

    desc.isShip = true;
    m_ShipShaders = std::make_unique<MeshletShaderSet>(m_Device, *m_ShaderFactory, desc);

    desc.taskShader = "asteroidTS";
    desc.meshShader = "asteroidMS";
    desc.isShip = false;
    desc.asteroids = true;
    desc.hiZ = (m_Role == MeshletRenderRole::GBuffer);
    m_AsteroidShaders = std::make_unique<MeshletShaderSet>(m_Device, *m_ShaderFactory, desc);
}

void MeshletDrawStrategy::PrepareForView(const std::shared_ptr<engine::SceneGraphNode>&, const engine::IView&)
{
    // vfunc01 (0x1400448D0): resets the (always empty) item cursor at +72.
}

const render::DrawItem* MeshletDrawStrategy::GetNextItem()
{
    // vfunc02 (0x140041240): returns the empty cursor.
    return nullptr;
}

void MeshletDrawStrategy::ResetFrameStatistics()
{
    // Asteroids.exe: 0x1400448B0
    g_NumMeshletDraws = 0;
    g_NumAsteroidInstances = 0;
    g_NumAsteroidPrimitives = 0;
}

uint32_t MeshletDrawStrategy::GetNumMeshletDraws() { return g_NumMeshletDraws; }
uint64_t MeshletDrawStrategy::GetNumAsteroidInstances() { return g_NumAsteroidInstances; }
uint64_t MeshletDrawStrategy::GetNumAsteroidPrimitives() { return g_NumAsteroidPrimitives; }

const std::vector<uint32_t>& MeshletDrawStrategy::ReadStatsReadback()
{
    // Inlined in FeatureDemo::RenderScene (0x1400342A0) after the frame was submitted.
    if (const void* data = m_Device->mapBuffer(m_StatsReadbackBuffer, nvrhi::CpuAccessMode::Read))
    {
        std::memcpy(m_StatsReadbackData.data(), data, m_StatsReadbackData.size() * sizeof(uint32_t));
        m_Device->unmapBuffer(m_StatsReadbackBuffer);
    }
    return m_StatsReadbackData;
}

void MeshletDrawStrategy::Render(nvrhi::ICommandList* commandList, MeshletPassState& passState,
    const engine::IView& view, const MeshletMaterialCallback& setupMaterial)
{
    // Asteroids.exe: MeshletDrawStrategy::vfunc00 (0x1400435A0)
    const engine::IView& extentView = lodView ? *lodView : view;
    const nvrhi::Rect extent = extentView.GetViewExtent();
    const float rtWidth = float(extent.maxX - extent.minX);
    const float rtHeight = float(extent.maxY - extent.minY);

    meshlet_shader::FrameCB frame = {};
    frame.matWorldToClip = view.GetViewProjectionMatrix(true);
    frame.matWorldToView = affineToHomogeneous(view.GetViewMatrix());
    frame.matViewToClip = view.GetProjectionMatrix(true);
    frame.matProjectionForLod = view.GetProjectionMatrix(false);
    frame.cameraPos = float4(view.GetViewOrigin(), 1.f);
    frame.blueNoise = c_BlueNoise;
    frame.preViewTranslation = float4(preViewTranslation, 0.f);
    frame.preViewTranslationPrevious = float4(preViewTranslationPrevious, 0.f);

    const frustum viewFrustum = view.GetViewFrustum();
    for (int plane = 0; plane < frustum::PLANES_COUNT; ++plane)
        frame.frustumPlanes[plane] = float4(viewFrustum.planes[plane].normal, viewFrustum.planes[plane].distance);

    frame.rtDims = float2(rtWidth, rtHeight);
    frame.lodBias = lodBias;
    frame.lodSlope = lodSlope;
    if (m_HiZTexture)
    {
        const nvrhi::TextureDesc& hiZDesc = m_HiZTexture->getDesc();
        frame.invZFarDims = float2(1.f / float(hiZDesc.width), 1.f / float(hiZDesc.height));
        frame.zFarNumLevels = float(hiZDesc.mipLevels);
    }
    frame.invZFarTileSize = c_InvZFarTileSize;
    frame.enableLod = enableLod;
    frame.alphaPreset = alphaPreset;
    frame.forcedLod = forcedLod;
    frame.enableDistanceLod = enableDistanceLod;
    frame.visualizeLods = visualizeLods;
    frame.transitionRange = transitionRange;
    frame.enableAsteroidsCulling = enableAsteroidsCulling;
    frame.showBBoxes = showBBoxes;
    frame.enableViewDistanceFade = (m_Role != MeshletRenderRole::Depth);
    frame.time = time;
    frame.orthographicProjection = view.IsOrthographicProjection();
    frame.enableZCull = enableZCull;
    frame.enableZCullStats = enableZCullStats;
    commandList->writeBuffer(m_Resources->frameConstants, &frame, sizeof(frame));

    commandList->setEnableUavBarriersForBuffer(m_Resources->debugUAV, false);
    commandList->setEnableUavBarriersForBuffer(m_Resources->statsUAV, false);
    if (m_Resources->nvExtensionBuffer)
        commandList->setEnableUavBarriersForBuffer(m_Resources->nvExtensionBuffer, false);

    if (drawPlayerShip || drawOtherObjects)
    {
        demo::ProfBegin(commandList, "BasicObjects");
        const size_t numObjects = m_Scene->GetObjects().size();
        for (size_t index = 0; index < numObjects; ++index)
        {
            std::shared_ptr<SpaceObject> object = m_Scene->GetSpaceObject(index);
            if (!object)
                continue;

            if (object->isPlayerShip ? drawPlayerShip : drawOtherObjects)
                DrawSpaceObject(commandList, passState, setupMaterial, object);
        }
        demo::ProfEnd(commandList);
    }

    if (drawAsteroids && !transparentPass)
    {
        demo::ProfBegin(commandList, "Asteroids");
        RenderAsteroids(commandList, passState, view, setupMaterial, frame.matProjectionForLod, rtWidth, rtHeight);
        demo::ProfEnd(commandList);
    }

    commandList->setEnableUavBarriersForBuffer(m_Resources->debugUAV, true);
    commandList->setEnableUavBarriersForBuffer(m_Resources->statsUAV, true);
    if (m_Resources->nvExtensionBuffer)
        commandList->setEnableUavBarriersForBuffer(m_Resources->nvExtensionBuffer, true);

    if (enableZCullStats)
    {
        commandList->copyBuffer(m_StatsReadbackBuffer, 0, m_Resources->statsUAV, 0,
            c_StatsReadbackEntries * sizeof(uint32_t));
    }
}

void MeshletDrawStrategy::RenderAsteroids(nvrhi::ICommandList* commandList, MeshletPassState& passState,
    const engine::IView& view, const MeshletMaterialCallback& setupMaterial, const float4x4& projectionForLod,
    float rtWidth, float rtHeight)
{
    // Asteroids.exe: second half of vfunc00 (0x1400435A0)
    numSectorsDrawn = 0;
    numAsteroidInstances = 0;
    numAsteroidPrimitives = 0;

    const AsteroidLibrary& library = m_Scene->GetAsteroidLibrary();

    // Sector range covered by the view frustum (scene space), grown by one sector in every direction.
    const frustum viewFrustum = view.GetViewFrustum();
    int2 minSector = int2(INT_MAX);
    int2 maxSector = int2(INT_MIN);
    for (int corner = 0; corner < 8; ++corner)
    {
        const int2 sector = m_Scene->GetSectorIndex(viewFrustum.getCorner(corner) - preViewTranslation);
        minSector = min(minSector, sector);
        maxSector = max(maxSector, sector);
    }
    minSector -= 1;
    maxSector += 1;

    // Shadow maps skip asteroid types whose largest instance is below 1.5% of the near-plane edge.
    float minRadius = 0.f;
    if (m_Role == MeshletRenderRole::Depth)
        minRadius = length(viewFrustum.getCorner(0) - viewFrustum.getCorner(1)) * 0.015f;

    std::vector<int2> visibleSectors;
    for (int z = minSector.y; z <= maxSector.y; ++z)
    {
        for (int x = minSector.x; x <= maxSector.x; ++x)
        {
            const int2 coords(x, z);
            std::shared_ptr<SpaceSector> sector = m_Scene->GetSector(coords);
            if (!sector)
                continue;

            const float3 origin = m_Scene->GetSectorOrigin(coords) + preViewTranslation;
            const box3 bounds = sector->GetBounds();
            if (view.IsBoxVisible(box3(bounds.m_mins + origin, bounds.m_maxs + origin)))
                visibleSectors.push_back(coords);
        }
    }

    // Nearest sectors first (squared distance in sector units to the camera's sector).
    const int2 cameraSector = m_Scene->GetSectorIndex(view.GetViewOrigin() - preViewTranslation);
    std::sort(visibleSectors.begin(), visibleSectors.end(), [cameraSector](const int2& a, const int2& b)
    {
        const int2 da = cameraSector - a;
        const int2 db = cameraSector - b;
        return da.x * da.x + da.y * da.y < db.x * db.x + db.y * db.y;
    });

    // Projected diameter scale for the minimum screen size test (same terms as the task shader's LOD).
    const float projectionScale = std::max(projectionForLod[1].y * rtHeight, projectionForLod[0].x * rtWidth);

    bool hiZBuilt = false;
    uint32_t sectorOrdinal = 0;
    uint32_t drawIndex = 0;
    const uint32_t numTypes = library.GetNumTypes();

    for (const int2& coords : visibleSectors)
    {
        // The Hi-Z pyramid is built from the depth of the nearest sectors, then the remaining sectors use the
        // occlusion-culling task shader.
        if (enableZCull && !hiZBuilt && m_Role == MeshletRenderRole::GBuffer && sectorOrdinal == zCullSectorThreshold
            && m_HiZPass)
        {
            m_HiZPass->Dispatch(commandList, uint32_t(rtWidth), uint32_t(rtHeight));
            hiZBuilt = true;
        }

        std::shared_ptr<SpaceSector> sector = m_Scene->GetSector(coords);
        const float3 origin = m_Scene->GetSectorOrigin(coords) + preViewTranslation;

        const size_t sectorInstances = sector->GetTotalInstanceCount();
        ++numSectorsDrawn;
        numAsteroidInstances += sectorInstances;
        g_NumAsteroidInstances += sectorInstances;

        // The 2018 loop ran over the library's types; the sectors are created with the same count.
        const uint32_t numSectorTypes = std::min(numTypes, sector->GetNumTypes());
        for (uint32_t typeId = 0; typeId < numSectorTypes; ++typeId)
        {
            const uint32_t instanceCount = sector->GetInstanceCount(typeId);
            if (instanceCount == 0)
                continue;

            const float maxRadius = sector->GetMaxRadius(typeId);
            if (maxRadius < minRadius)
                continue;

            if (minAsteroidScreenSize > 0.f)
            {
                const float projectedSize = maxRadius / std::max(length(origin), 1.f) * projectionScale;
                if (projectedSize < minAsteroidScreenSize)
                    continue;
            }

            const uint64_t primitives = uint64_t(instanceCount) * library.GetNumPrims(typeId);
            numAsteroidPrimitives += primitives;
            g_NumAsteroidPrimitives += primitives;

            nvrhi::IBindingSet* sectorBindingSet = GetOrCreateSectorBindingSet(*sector, typeId);
            if (!sectorBindingSet)
                continue;

            DrawAsteroidType(commandList, passState, setupMaterial, typeId, sectorBindingSet, origin, instanceCount,
                drawIndex++, hiZBuilt);
        }

        ++sectorOrdinal;
    }
}

void MeshletDrawStrategy::DrawAsteroidType(nvrhi::ICommandList* commandList, MeshletPassState& passState,
    const MeshletMaterialCallback& setupMaterial, uint32_t typeId, nvrhi::IBindingSet* sectorBindingSet,
    const float3& sectorOffset, uint32_t instanceCount, uint32_t drawIndex, bool useHiZ)
{
    // Asteroids.exe: 0x1400415A0
    std::shared_ptr<AsteroidType> type = m_Scene->GetAsteroidLibrary().GetAsteroidType(typeId);
    if (!type)
        return;

    nvrhi::IBindingSet* typeBindingSet = GetOrCreateAsteroidTypeBindingSet(*type);
    if (!typeBindingSet)
        return;

    meshlet_shader::SectorInfo sectorInfo = {};
    sectorInfo.sectorOffset = sectorOffset;
    sectorInfo.asteroidId = drawIndex;
    commandList->writeBuffer(m_Resources->sectorConstants, &sectorInfo, sizeof(sectorInfo));

    // The asteroid path ignores the callback result.
    if (setupMaterial)
        setupMaterial(type->GetMaterial(), passState);

    const uint32_t variant = (useHiZ ? MeshletShaderSet::Variant_HiZ : 0)
        | (wireframe ? MeshletShaderSet::Variant_Wireframe : 0);

    const MeshletPipelineRef pipeline = GetOrCreatePipeline(*m_AsteroidShaders, variant, passState,
        m_Resources->asteroidTypeBindingLayout, m_Resources->asteroidSectorBindingLayout);
    if (!pipeline)
        return;

    nvrhi::BindingSetVector bindings = passState.bindings;
    bindings.push_back(typeBindingSet);
    bindings.push_back(sectorBindingSet);

    // One task group per asteroid instance (asteroidTS indexes instanceInfoBuffer with the group id).
    DispatchMeshlets(commandList, pipeline, passState, bindings, instanceCount);
    ++g_NumMeshletDraws;
}

void MeshletDrawStrategy::DrawSpaceObject(nvrhi::ICommandList* commandList, MeshletPassState& passState,
    const MeshletMaterialCallback& setupMaterial, const std::shared_ptr<SpaceObject>& object)
{
    // Asteroids.exe: 0x140042400
    nvrhi::IBindingSet* objectBindingSet = GetOrCreateSpaceObjectBindingSet(*object);
    if (!objectBindingSet)
        return;

    // cbObjectInfo of a space object: the quantization box of the meshlet culling data (SpaceObject+588), and the
    // ship glow parameters (SpaceObject+560..+572) in the fields the asteroid shaders use for the bounding sphere
    // and LOD bias.
    meshlet_shader::ObjectConstants objectConstants = {};
    const box3& bounds = object->GetMeshletBounds();
    objectConstants.bbox.bboxMin = float4(bounds.m_mins, 1.f);
    objectConstants.bbox.bboxMax = float4(bounds.m_maxs, 1.f);
    objectConstants.center = float3(object->shipGlowParams.y, object->shipGlowParams.z, object->shipGlowParams.w);
    objectConstants.radius = 0.f;
    objectConstants.lodBias = object->shipGlowParams.x;
    objectConstants.maxLevelToRender = 0.f;
    commandList->writeBuffer(m_Resources->objectConstants, &objectConstants, meshlet_shader::c_ObjectConstantsUploadSize);

    const affine3 cameraOffset = translation(preViewTranslation);
    const affine3 previousCameraOffset = translation(preViewTranslationPrevious);

    MeshletPipelineRef pipeline;
    nvrhi::BindingSetVector bindings;
    const SceneMaterial* currentMaterial = nullptr;

    for (SceneMeshInstance* instance : object->GetMeshInstances())
    {
        SceneMaterial* material = (instance && instance->mesh) ? instance->mesh->material : nullptr;
        if (!material)
            continue;

        // Opaque (0) and 1 go to the non-forward strategies, Transparent (2) to the forward one.
        const int domain = int(material->domain);
        const bool drawInThisPass = (domain == 0 || domain == 1) ? !transparentPass
            : (domain == int(SceneMaterialDomain::Transparent) && transparentPass);
        if (!drawInThisPass)
            continue;

        if (material != currentMaterial)
        {
            if (setupMaterial && !setupMaterial(material, passState))
                continue;

            MeshletShaderSet& shaderSet = IsShipMaterialName(material->name) ? *m_ShipShaders : *m_BasicShaders;
            const uint32_t variant = transparentPass ? MeshletShaderSet::Variant_AlphaBlend : 0;

            pipeline = GetOrCreatePipeline(shaderSet, variant, passState, m_Resources->basicObjectBindingLayout, nullptr);
            if (!pipeline)
                continue;

            bindings = passState.bindings;
            bindings.push_back(objectBindingSet);

            currentMaterial = material;
        }

        meshlet_shader::MeshletInfoCB meshletInfo = {};
        meshletInfo.numMeshlets = instance->numMeshlets;
        meshletInfo.firstMeshlet = instance->firstMeshlet;
        commandList->writeBuffer(m_Resources->meshletInfoConstants, &meshletInfo, sizeof(meshletInfo));

        meshlet_shader::InstanceCB instanceConstants = {};
        instanceConstants.instanceMat = ToShaderInstanceMatrix(instance->transform * cameraOffset);
        commandList->writeBuffer(m_Resources->instanceConstants, &instanceConstants, sizeof(instanceConstants));

        meshlet_shader::InstanceCB previousInstanceConstants = {};
        previousInstanceConstants.instanceMat = ToShaderInstanceMatrix(instance->previousTransform * previousCameraOffset);
        commandList->writeBuffer(m_Resources->instanceConstantsPrevious, &previousInstanceConstants, sizeof(previousInstanceConstants));

        // basicTS launches cbMeshletInfo.numMeshlets mesh groups.
        DispatchMeshlets(commandList, pipeline, passState, bindings, 1);
        ++g_NumMeshletDraws;
    }
}

MeshletPipelineRef MeshletDrawStrategy::GetOrCreatePipeline(MeshletShaderSet& shaderSet, uint32_t variant,
    const MeshletPassState& passState, nvrhi::IBindingLayout* meshletLayout0, nvrhi::IBindingLayout* meshletLayout1)
{
    if (!passState.framebuffer)
        return {};

    const nvrhi::FramebufferInfo& framebufferInfo = passState.framebuffer->getFramebufferInfo();

    nvrhi::BindingLayoutVector layouts = passState.bindingLayouts;
    if (meshletLayout0)
        layouts.push_back(meshletLayout0);
    if (meshletLayout1)
        layouts.push_back(meshletLayout1);
    // NVAPI mode: the extension UAV (u7, space0) is part of the root signature (2018: item of the meshlet layouts).
    if (shaderSet.GetMode() == MeshShaderMode::Nvapi)
    {
        if (!m_Resources->nvExtensionBindingLayout)
            return {};
        layouts.push_back(m_Resources->nvExtensionBindingLayout);
    }

    // deviation: the 2018 cache was keyed by the variant only; the key also holds the pass render state, the
    // binding layouts and the framebuffer formats (see MeshletShaderSet::GetPipeline).
    if (MeshletPipelineRef pipeline = shaderSet.GetPipeline(variant, passState.renderState, layouts, framebufferInfo))
        return pipeline;

    return shaderSet.CreatePipeline(variant, passState.renderState, layouts, framebufferInfo);
}

void MeshletDrawStrategy::DispatchMeshlets(nvrhi::ICommandList* commandList, const MeshletPipelineRef& pipeline,
    const MeshletPassState& passState, const nvrhi::BindingSetVector& bindings, uint32_t groupCount)
{
    if (pipeline.nvapi)
    {
        // 2018 (0x1400415A0 / 0x140042400): ICommandList::setGraphicsState with the wrapped NVAPI PSO, then the NVAPI
        // DispatchMeshTasks (0x140001AD0) on the native command list. The 2018 nvrhi patched volatile constant
        // buffer addresses in setGraphicsState / writeBuffer; current nvrhi does it in its draw calls, so it is
        // triggered explicitly (the 2018 particle code called an nvrhi function for the same purpose, 0x1401A9320).
        nvrhi::GraphicsState state;
        state.setPipeline(pipeline.nvapi)
            .setFramebuffer(passState.framebuffer)
            .setViewport(passState.viewport);
        state.bindings = bindings;
        state.addBindingSet(m_Resources->nvExtensionBindingSet);
        commandList->setGraphicsState(state);

        nvrhi::d3d12::ICommandList* d3d12CommandList = commandList->getNativeObject(nvrhi::ObjectTypes::Nvrhi_D3D12_CommandList);
        if (d3d12CommandList)
            d3d12CommandList->updateGraphicsVolatileBuffers();

        NvDispatchMeshTasks(commandList, groupCount);
        return;
    }

    nvrhi::MeshletState state;
    state.setPipeline(pipeline.meshlet)
        .setFramebuffer(passState.framebuffer)
        .setViewport(passState.viewport);
    state.bindings = bindings;
    commandList->setMeshletState(state);
    commandList->dispatchMesh(groupCount);
}

nvrhi::IBindingSet* MeshletDrawStrategy::GetOrCreateAsteroidTypeBindingSet(AsteroidType& type)
{
    // Asteroids.exe: first part of 0x1400415A0 (AsteroidType+648 cache).
    nvrhi::BindingSetHandle& slot = type.GetMeshletBindingSet();
    if (slot)
        return slot;

    MeshletRenderResources& res = *m_Resources;

    nvrhi::BindingSetDesc setDesc;
    for (uint32_t id : { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 8u, 10u })
        setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(id, type.GetBuffer(AsteroidBufferType(id))));
    setDesc.addItem(nvrhi::BindingSetItem::RawBuffer_SRV(7, type.GetBuffer(AsteroidBufferType::TriangleIndices)));
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(15, m_RandomsTexture));
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(16, m_ViewDistanceTexture));    // t_ViewDistance
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(17, m_HiZTexture));             // t_ZFar
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, res.frameConstants));     // cbFrame
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(4, type.GetObjectConstantsBuffer())); // cbObjectInfo
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(5, res.sectorConstants));    // cbSectorInfo
    // deviation: the 2018 set left u0 / u1 empty (null resources), so the task shader's u_Stats counter only
    // worked if the pass bound it; bind DebugUAV / StatsUAV here.
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, res.debugUAV));
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, res.statsUAV));    // u_Stats
    setDesc.addItem(nvrhi::BindingSetItem::Sampler(0, res.linearClampSampler));        // s_ViewDistanceSampler
    setDesc.addItem(nvrhi::BindingSetItem::Sampler(1, res.maxReductionSampler));       // s_ZFarSampler

    slot = CreateBindingSet(m_Device, setDesc, res.asteroidTypeBindingLayout);
    if (!slot)
        log::error("MeshletDrawStrategy: failed to create the binding set of asteroid type '%s'", type.GetName().c_str());
    return slot;
}

nvrhi::IBindingSet* MeshletDrawStrategy::GetOrCreateSectorBindingSet(SpaceSector& sector, uint32_t typeId)
{
    // Asteroids.exe: inlined in vfunc00 (0x1400435A0), sector+48 [typeId] cache.
    nvrhi::BindingSetHandle& slot = sector.GetBindingSet(typeId);
    if (slot)
        return slot;

    nvrhi::IBuffer* instanceBuffer = sector.GetInstanceBuffer(typeId);
    if (!instanceBuffer)
        return nullptr;

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(11, instanceBuffer));  // instanceInfoBuffer
    slot = CreateBindingSet(m_Device, setDesc, m_Resources->asteroidSectorBindingLayout);
    return slot;
}

nvrhi::IBindingSet* MeshletDrawStrategy::GetOrCreateSpaceObjectBindingSet(SpaceObject& object)
{
    // Asteroids.exe: first part of 0x140042400 (SpaceObject+616 cache).
    nvrhi::BindingSetHandle& slot = object.GetMeshletBindingSet();
    if (slot)
        return slot;

    MeshletRenderResources& res = *m_Resources;

    nvrhi::BindingSetDesc setDesc;
    for (uint32_t id : { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 8u })
        setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(id, object.GetMeshletBuffer(SpaceObjectBufferType(id))));
    setDesc.addItem(nvrhi::BindingSetItem::RawBuffer_SRV(7, object.GetMeshletBuffer(SpaceObjectBufferType::TriangleIndices)));
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(15, m_RandomsTexture));
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(17, m_HiZTexture));
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, res.frameConstants));             // cbFrame
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(1, res.meshletInfoConstants));       // cbMeshletInfo
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(2, res.instanceConstants));          // cbInstance
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(3, res.instanceConstantsPrevious));  // cbInstancePrev
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(4, res.objectConstants));            // cbObjectInfo
    // deviation: null u0 / u1 in 2018 (see GetOrCreateAsteroidTypeBindingSet).
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, res.debugUAV));
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, res.statsUAV));

    slot = CreateBindingSet(m_Device, setDesc, res.basicObjectBindingLayout);
    if (!slot)
        log::error("MeshletDrawStrategy: failed to create a space object binding set");
    return slot;
}

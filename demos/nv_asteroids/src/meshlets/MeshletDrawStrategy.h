#pragma once

#include "meshlets/HiZPass.h"
#include "meshlets/MeshletShaderSet.h"

#include <donut/core/math/math.h>
#include <donut/render/DrawStrategy.h>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace donut::engine
{
    class IView;
    class ShaderFactory;
}

class AsteroidType;
class MeshletRenderResources;
class SpaceObject;
class SpaceScene;
class SpaceSector;
struct SceneMaterial;
struct SceneMeshInstance;

// Constructor argument 'role' (2018 +248). FeatureDemo::CreateRenderPasses (0x14002BD60) builds five strategies:
// +248 gbuffer_ps (GBuffer), +264 no PS (Depth: shadow maps), +280 material_id_ps (MaterialId),
// +296 forward_ps (Forward, transparentPass = true) and +312 forward_ps (Forward: the ship overlay).
enum class MeshletRenderRole : int32_t
{
    Depth = 0,          // DEPTH_PRE_PASS=1, depth bias, no view-distance fade, small asteroids skipped
    GBuffer = 1,        // owns the Hi-Z pass; asteroid TS with _MESHLETS_HI_Z
    MaterialId = 2,
    Forward = 3,
    // unresolved: role 4 is handled by the constructor (ship and object drawing disabled) but never created.
    AsteroidsOnly = 4,
};

// State supplied by the calling pass for one Render() call.
// deviation: the 2018 passes handed over an nvrhi::GraphicsState whose pipeline was the pass's own graphics pipeline;
// the strategy copied that pipeline's desc (render state, binding layouts) and framebuffer info into each mesh
// shading PSO. With nvrhi meshlet pipelines the pass passes the render state and its layouts directly.
struct MeshletPassState
{
    nvrhi::IFramebuffer* framebuffer = nullptr;
    nvrhi::ViewportState viewport;
    nvrhi::RenderState renderState;             // blend / depth-stencil / raster state of the pass
    nvrhi::BindingLayoutVector bindingLayouts;  // pass layouts (space0, e.g. the material layout); meshlet layouts are appended
    nvrhi::BindingSetVector bindings;           // sets for 'bindingLayouts', same order
};

// Material hook of the pass (2018: std::function<bool(Material*)> that also edited the GraphicsState it captured).
// Called whenever the material changes: once per asteroid type draw (result ignored) and for every material change
// while drawing a space object's mesh instances (returning false skips the instance). It may replace entries of
// state.bindings (e.g. the material binding set) before they are copied into the meshlet state.
using MeshletMaterialCallback = std::function<bool(SceneMaterial* material, MeshletPassState& state)>;

// Asteroids.exe: MeshletDrawStrategy (vtable 0x140259970, 0x178 bytes; ctor 0x1400400E0, created through
// std::make_shared by 0x14001BF90). Draws the meshlet geometry of the scene: the space objects (player ship and
// other models, basicTS/basicMS) and the asteroid field (sector grid x asteroid types, asteroidTS/asteroidMS).
//
// The 2018 class drove NVAPI task/mesh shaders; this one does so in the NVAPI mesh-shader mode and uses D3D12
// SM6.5 amplification/mesh shaders via nvrhi meshlet pipelines in the D3D12 mode (see MeshShaderMode.h). The generic IDrawStrategy interface yields no items (as in 2018: vfunc01 resets an
// empty cursor and vfunc02 returns it), passes call Render() instead.
class MeshletDrawStrategy : public donut::render::IDrawStrategy
{
public:
    // 0x1400400E0. 'depthBuffer' / 'hiZBuffer' are render targets 0 and 1 (2018 RenderTargets object),
    // 'randomsTexture' is media/randoms_texture.dds (FeatureDemo+1056), 'viewDistanceTexture' the 1D
    // "ViewDistanceMap" (FeatureDemo+1176), 'pixelShaderFile' e.g. "gbuffer_ps.hlsl" (empty for the depth role).
    MeshletDrawStrategy(
        nvrhi::IDevice* device,
        std::shared_ptr<donut::engine::ShaderFactory> shaderFactory,
        std::shared_ptr<SpaceScene> scene,
        std::shared_ptr<MeshletRenderResources> resources,
        nvrhi::ITexture* depthBuffer,
        nvrhi::ITexture* hiZBuffer,
        nvrhi::ITexture* randomsTexture,
        nvrhi::ITexture* viewDistanceTexture,
        const std::string& pixelShaderFile,
        MeshletRenderRole role);

    // vfunc00 (0x1400435A0): fills cbFrame, then draws "BasicObjects" and "Asteroids".
    void Render(nvrhi::ICommandList* commandList, MeshletPassState& passState, const donut::engine::IView& view,
        const MeshletMaterialCallback& setupMaterial);

    // 0x1400412D0: (re)creates the three shader sets; FeatureDemo calls it after a shader reload.
    void CreateShaders();

    // IDrawStrategy (vfunc01 0x1400448D0 / vfunc02 0x140041240)
    void PrepareForView(const std::shared_ptr<donut::engine::SceneGraphNode>& rootNode, const donut::engine::IView& view) override;
    const donut::render::DrawItem* GetNextItem() override;

    // FeatureDemo's "numCulled" readback (RenderScene, when enableZCullStats): maps the "DebugUAVDest" copy of
    // StatsUAV made at the end of Render(); [0] = asteroids rejected by the Hi-Z test.
    const std::vector<uint32_t>& ReadStatsReadback();

    // Frame-global statistics (0x1400448B0 reset, getters 0x140041220 / 0x140041210 / 0x140041230).
    static void ResetFrameStatistics();
    static uint32_t GetNumMeshletDraws();       // dword_1402DF260: dispatches of all strategies
    static uint64_t GetNumAsteroidInstances();  // qword_1402DF268
    static uint64_t GetNumAsteroidPrimitives(); // qword_1402DF270: instances x u8 primitive indices of the last LOD

    [[nodiscard]] MeshletRenderRole GetRole() const { return m_Role; }

    // ---------------------------------------------------------------------------------------------------------
    // Settings written by FeatureDemo (UpdateAsteroidRendererSettings 0x140037370, shadows 0x140034F70,
    // CreateRenderPasses 0x14002BD60). The comment gives the 2018 field offset; see src/meshlets/NOTES.md.
    // ---------------------------------------------------------------------------------------------------------
    bool transparentPass = false;           // +8   (int) forward: only Transparent materials, no asteroids
    bool wireframe = false;                 // +252 pipeline variant bit 1 (asteroids only)
    bool enableLod = true;                  // +253 cbFrame.enableLod
    bool enableDistanceLod = true;          // +254 cbFrame.enableDistanceLod
    bool enableAsteroidsCulling = true;     // +255 cbFrame.enableAsteroidsCulling
    bool showBBoxes = false;                // +256 cbFrame.showBBoxes
    bool enableZCullStats = false;          // +257 cbFrame.enableZCullStats + StatsUAV readback copy
    uint32_t alphaPreset = 0;               // +260 cbFrame.alphaPreset
    uint32_t forcedLod = 0;                 // +264 cbFrame.forcedLod
    uint32_t visualizeLods = 0;             // +268 cbFrame.visualizeLods
    uint32_t zCullSectorThreshold = 3;      // +272 sectors drawn (nearest first) before the Hi-Z pyramid is built
    bool enableZCull = false;               // +276 cbFrame.enableZCull; builds Hi-Z and uses the HI_Z task shader
    float transitionRange = 0.1f;           // +280 cbFrame.transitionRange (LOD cross-fade)
    float lodBias = 0.f;                    // +284 cbFrame.lodBias
    float lodSlope = 1.f;                   // +288 cbFrame.lodSlope
    const donut::engine::IView* lodView = nullptr; // +328 view whose extent drives rtDims (FeatureDemo: main view)
    bool drawAsteroids = true;              // +336
    bool drawPlayerShip = true;             // +337 space objects with isPlayerShip
    bool drawOtherObjects = true;           // +338 the other space objects
    donut::math::float3 preViewTranslation = 0.f;          // +340 camera-relative rendering offset (cbFrame.preViewTranslation)
    donut::math::float3 preViewTranslationPrevious = 0.f;  // +352 previous frame (cbFrame.preViewTranslationPrevious)
    float time = 0.f;                       // +364 cbFrame.time
    float minAsteroidScreenSize = 0.f;      // +368 skip asteroid types whose largest instance projects smaller (pixels); 0 = off

    // Statistics of the last Render() with asteroids (read by FeatureDemo from the G-buffer strategy).
    uint32_t numSectorsDrawn = 0;           // +304
    uint64_t numAsteroidInstances = 0;      // +312
    uint64_t numAsteroidPrimitives = 0;     // +320

private:
    void RenderAsteroids(nvrhi::ICommandList* commandList, MeshletPassState& passState, const donut::engine::IView& view,
        const MeshletMaterialCallback& setupMaterial, const donut::math::float4x4& projectionForLod, float rtWidth, float rtHeight);

    // 0x1400415A0
    void DrawAsteroidType(nvrhi::ICommandList* commandList, MeshletPassState& passState,
        const MeshletMaterialCallback& setupMaterial, uint32_t typeId, nvrhi::IBindingSet* sectorBindingSet,
        const donut::math::float3& sectorOffset, uint32_t instanceCount, uint32_t drawIndex, bool useHiZ);

    // 0x140042400
    void DrawSpaceObject(nvrhi::ICommandList* commandList, MeshletPassState& passState,
        const MeshletMaterialCallback& setupMaterial, const std::shared_ptr<SpaceObject>& object);

    nvrhi::IBindingSet* GetOrCreateAsteroidTypeBindingSet(AsteroidType& type);
    nvrhi::IBindingSet* GetOrCreateSectorBindingSet(SpaceSector& sector, uint32_t typeId);
    nvrhi::IBindingSet* GetOrCreateSpaceObjectBindingSet(SpaceObject& object);

    MeshletPipelineRef GetOrCreatePipeline(MeshletShaderSet& shaderSet, uint32_t variant,
        const MeshletPassState& passState, nvrhi::IBindingLayout* meshletLayout0, nvrhi::IBindingLayout* meshletLayout1);

    // Binds the pipeline and launches 'groupCount' task groups: nvrhi dispatchMesh (D3D12 mode) or setGraphicsState
    // + NVAPI DispatchMeshTasks (NVAPI mode).
    void DispatchMeshlets(nvrhi::ICommandList* commandList, const MeshletPipelineRef& pipeline,
        const MeshletPassState& passState, const nvrhi::BindingSetVector& bindings, uint32_t groupCount);

    MeshletRenderRole m_Role;                                   // +248
    std::unique_ptr<HiZPass> m_HiZPass;                         // +16  (G-buffer role only)
    std::shared_ptr<SpaceScene> m_Scene;                       // +24
    nvrhi::DeviceHandle m_Device;                               // +40 (2018: the nvrhi D3D12 device object)
    std::shared_ptr<donut::engine::ShaderFactory> m_ShaderFactory; // +160
    std::shared_ptr<MeshletRenderResources> m_Resources;        // +200
    std::string m_PixelShaderFile;                              // +216

    // +88 / +112: "DebugUAVDest" readback buffers with their CPU copies (0x10000 x 20 bytes for DebugUAV, 16 x uint
    // for StatsUAV).
    std::vector<uint8_t> m_DebugReadbackData;
    nvrhi::BufferHandle m_DebugReadbackBuffer;                  // +104
    std::vector<uint32_t> m_StatsReadbackData;
    nvrhi::BufferHandle m_StatsReadbackBuffer;                  // +128

    nvrhi::TextureHandle m_RandomsTexture;                      // +136 t15
    nvrhi::TextureHandle m_ViewDistanceTexture;                 // +144 t16
    nvrhi::TextureHandle m_HiZTexture;                          // +152 t17

    std::unique_ptr<MeshletShaderSet> m_BasicShaders;           // +176 basicTS / basicMS, IS_SHIP=0
    std::unique_ptr<MeshletShaderSet> m_ShipShaders;            // +184 basicTS / basicMS, IS_SHIP=1
    std::unique_ptr<MeshletShaderSet> m_AsteroidShaders;        // +192 asteroidTS / asteroidMS, _ASTEROIDS=1
};

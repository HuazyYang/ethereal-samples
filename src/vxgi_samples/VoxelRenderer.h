#ifndef SRC_VXGISAMPLE_VXGIRENDERER_H
#define SRC_VXGISAMPLE_VXGIRENDERER_H
#include "VXGIIntTypes.h"
#include <nvrhi/core/foundation.h>
#include <nvrhi/core/autoptr.h>
#include <donut/render/DrawStrategy.h>
#include <nvrhi/nvrhi.h>
#include "ClipmapGeometry.h"
#include <map>

namespace donut::engine {
class ShaderFactory;
class BindingCache;
}

namespace vxgi {

// nvrhi requires a binding set to bind every item of its layout, and it has no null textures. The VXGI passes
// share one binding layout per subsystem (like the original SDK's root signatures) while each dispatch binds only
// the resources its shader uses. BindingSetFactory binds an inert placeholder to every other slot of the layout.
// No shader reads one: the compilers drop resources a shader does not use, so those slots are not even declared
// by it. Every UAV slot gets a placeholder of its own (D3D11 rejects one resource in two UAV slots of a stage).
// Volatile constant buffers get no placeholder (one would have to be written in every command list), so a set
// that leaves one out is still reported.
class BindingSetFactory {
 public:
    explicit BindingSetFactory(nvrhi::IDevice *device);

    nvrhi::BindingSetDesc Complete(const nvrhi::BindingSetDesc &desc, nvrhi::IBindingLayout *layout);
    nvrhi::BindingSetHandle Create(const nvrhi::BindingSetDesc &desc, nvrhi::IBindingLayout *layout);
    nvrhi::BindingSetHandle GetOrCreate(donut::engine::BindingCache &cache, const nvrhi::BindingSetDesc &desc,
                                        nvrhi::IBindingLayout *layout);

 private:
    nvrhi::IRHIObject *GetPlaceholder(nvrhi::ResourceType type, uint32_t slot);

    nvrhi::IDevice *m_Device;
    // Keyed by (resource type, slot); placeholders that are not UAVs are shared by all slots (slot 0).
    std::map<std::pair<nvrhi::ResourceType, uint32_t>, nvrhi::AutoPtr<nvrhi::IRHIObject>> m_Placeholders;
};

class VoxelRenderer;
struct AllocationMap;
struct VoxelTexture;
class ViewTracer;

class VoxelRenderer : public nvrhi::ObjectImpl<nvrhi::IObject>  {
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(VoxelRenderer)
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IObject)
    NVRHI_END_INTERFACE_TABLE()

    VoxelRenderer(nvrhi::IDevice *device, donut::engine::ShaderFactory *shaderFactory);
    ~VoxelRenderer();

    // Resources are (re)allocated and initialized on commandList, which must be open.
    Status setVoxelizationParameters(nvrhi::ICommandList *commandList, const VoxelizationParameters &params,
                                     bool *invalidated);

    Status createNewTracer(ViewTracer **ppViewTracer, bool ambientOcclusion);

    const DerivedVoxelizationParameters *GetVoxelizationParameters();

    nvrhi::IDevice *GetDevice();
    donut::engine::ShaderFactory *GetShaderFactory();
    uint GetNumFramesInFlight();
    PerGPUData *GetCurrentPerGPUData();
    const ClipmapGeometry *GetClipmapGeometry();
    AllocationMap *GetAllocationMap();
    BindingSetFactory &GetBindingSetFactory() { return m_BindingSetFactory; }
    Status AllocateResources(nvrhi::ICommandList *commandList);
    void ReleaseResources();

    Status TranslateVoxelizationParameters(const VoxelizationParameters &inParams,
                                           DerivedVoxelizationParameters &outParams);
    Status createViewTracer(ViewTracer **ppTracer);
    float3 getLastUpdatedClipmapAnchor();
    box3 calculateHypotheticalWorldRegion(const float3 &clipmapAnchor, float giRange);
    Status prepareVoxelizationViewMatrix(const float3 &clipmapAnchor, float giRange,
                                         float4x4 *viewMatrix);
    Status prepareForOpacityVoxelization(nvrhi::ICommandList *commandList,
                                         const UpdateVoxelizationParameters *params,
                                         bool *performOpacityVoxelization,
                                         bool *performEmittanceVoxelization);
    Status prepareForEmittanceVoxelization();
    Status getInvalidatedRegion(box3 *pRegions, uint maxRegions, uint *numRegions);
    float getMinVoxelSizeAtPoint(const float3 &point, VoxelSizeFunction function,
                                 bool zeroOutOfRange);
    Status getVoxelizationViewMatrix(float4x4 *viewMatrix);

    Status getVoxelizationState(nvrhi::ICommandList *commandList, bool isEmittance, const MaterialInfo &materialInfo,
                                nvrhi::GraphicsState *state);
    Status updateVoxelizationMaterialParameters(nvrhi::ICommandList *commandList, const MaterialInfo &materialInfo);
    Status finalizeVoxelization(nvrhi::ICommandList *commandList);
    Status renderDebug(nvrhi::ICommandList *commandList, const DebugRenderParameters *params);
    const box3& getLastUpdatedWorldRegion();
    const box3 &getLastUpdatedSceneExtents();
    void InvalidateRegion(const box3 &boundingBox);
    void InvalidateRegionForCurrentGPU(const box3 &boundingBox);
    void InvalidateLevelMovementSlabs(uint level, const box3 &newRegion);
    void FillAbstractTracingConstants(nvrhi::ICommandList *commandList);
    void InvalidateLightFrustum(const frustum &f);

    void GetOpacityShaders(nvrhi::IShader **geometryShader, nvrhi::IShader **pixelShader);
    void GetEmittanceShaders(bool superSampling, nvrhi::IShader **geometryShader,
                             nvrhi::IShader **pixelShader);
    void GetBindingLayout(nvrhi::IBindingLayout **bindingLayout);
    nvrhi::IBuffer *GetAbstractTracingCB();
    nvrhi::IBuffer *GetTracingClipmapLevelCB();
    nvrhi::ISampler *GetLinearWrapSampler();
    VoxelTexture *GetVoxelTexture();

    nvrhi::IShader *GetFullScreenQuadVS();

    nvrhi::IFramebuffer *GetProjectedCoverageFramebuffer();

    uint GetUserShaderBindingSlot(UserShaderBindingID id);

 private:
    nvrhi::IDevice *m_Device;
    nvrhi::AutoPtr<donut::engine::ShaderFactory> m_ShaderFactory;
    BindingSetFactory m_BindingSetFactory;
    nvrhi::MonoPtr<AllocationMap> m_AllocationMap;
    nvrhi::MonoPtr<VoxelTexture> m_VoxelTexture;
    box3 m_SceneExtents;
    DerivedVoxelizationParameters m_Parameters;
    ClipmapGeometry m_ClipGeometry;
    bool m_FullRevoxelizationRequired;
    nvrhi::BufferHandle m_pCacheLevelsCB;
    nvrhi::BufferHandle m_pAbstractTracingCB;
    nvrhi::SamplerHandle m_pSamplerLinearWrap;
    IndirectIrradianceMapTracingParameters m_IndirectIrradianceMapTracingParameters;

    bool m_UpdateOpacity;
    bool m_UpdateEmittance;
    bool m_AnchorMoved;
    std::vector<box3> m_SnappedRegionsToInvalidate;

    PerGPUData m_PerGPUData;
    
    // UserDefined Pipelines
    nvrhi::BindingLayoutHandle m_VXGIBindingLayout;
    nvrhi::ShaderHandle m_VoxelizeGS;
    nvrhi::ShaderHandle m_VoxelizeGS_SWCoverage;
    nvrhi::ShaderHandle m_VoxelizeOpacityPS;
    nvrhi::ShaderHandle m_VoxelizeEmittancePS;
    nvrhi::ShaderHandle m_VoxelizeEmittancePS_SWCoverage;

    nvrhi::ShaderHandle m_FullScreenQuadVS;

    std::vector<nvrhi::Viewport> m_VoxelizeViewports;
    std::vector<nvrhi::Rect> m_VoxelizeScissorRects;

    std::vector<ViewTracer *> m_ViewTracers;

    nvrhi::FramebufferHandle m_DebugFramebuffer;
};

}  // namespace vxgi

#endif /* SRC_VXGISAMPLE_VXGIRENDERER_H */

#ifndef SRC_VXGISAMPLE_VXGIRENDERER_H
#define SRC_VXGISAMPLE_VXGIRENDERER_H
#include "VXGIIntTypes.h"
#include <donut/core/object/Foundation.h>
#include <donut/core/object/AutoPtr.h>
#include <donut/render/DrawStrategy.h>
#include <nvrhi/nvrhi.h>
#include "ClipmapGeometry.h"

namespace donut::engine {
class ShaderFactory;
}

namespace vxgi {

class VoxelRenderer;
struct AllocationMap;
struct VoxelTexture;
class ViewTracer;

class VoxelRenderer : public donut::ObjectImpl<donut::IObject>  {
 public:
    VoxelRenderer(nvrhi::IDevice *device, donut::engine::ShaderFactory *shaderFactory);
    ~VoxelRenderer();

    Status setVoxelizationParameters(const VoxelizationParameters &params, bool *invalidated);

    Status createNewTracer(ViewTracer **ppViewTracer, bool ambientOcclusion);

    const DerivedVoxelizationParameters *GetVoxelizationParameters();

    nvrhi::IDevice *GetDevice();
    donut::engine::ShaderFactory *GetShaderFactory();
    uint GetNumFramesInFlight();
    PerGPUData *GetCurrentPerGPUData();
    const ClipmapGeometry *GetClipmapGeometry();
    AllocationMap *GetAllocationMap();
    Status AllocateResources();
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
    donut::AutoPtr<donut::engine::ShaderFactory> m_ShaderFactory;
    nvrhi::CommandListHandle m_CommandList;
    donut::MonoPtr<AllocationMap> m_AllocationMap;
    donut::MonoPtr<VoxelTexture> m_VoxelTexture;
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

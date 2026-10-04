// ContextImp.h
//
// Platform independent context: owns the context/viewer descriptors and the temporal-filter state,
// fills the shader constant buffers and sequences the per-stage virtual hooks implemented by the
// platform backend (ContextImp_D3D11).

#pragma once

#include "Common.h"
#include "ShaderConstants.h"
#include "VectorMath.h"

namespace Nv
{
namespace VolumetricLighting
{

// NvVolumetricLighting.d3d11.dll: vtable 0x1800111D8 (16 slots: deleting dtor + 15 pure virtual stage hooks)
class ContextImp
{
public:
    explicit ContextImp(const ContextDesc* pContextDesc);
    virtual ~ContextImp();

    // Public entry points (called by the exported functions)
    Status BeginAccumulation(BeginAccumulationArgs* pArgs);
    Status RenderVolume(RenderVolumeArgs* pArgs);
    Status EndAccumulation(EndAccumulationArgs* pArgs);
    Status ApplyLighting(ApplyLightingArgs* pArgs);

protected:
    // Stage hooks, in vtable order (slots 1..15)
    virtual Status BeginAccumulation_Start(BeginAccumulationArgs* pArgs) = 0;
    virtual Status BeginAccumulation_UpdateMediumLUT(BeginAccumulationArgs* pArgs) = 0;
    virtual Status BeginAccumulation_CopyDepth(BeginAccumulationArgs* pArgs) = 0;
    virtual Status BeginAccumulation_End(BeginAccumulationArgs* pArgs) = 0;
    virtual Status RenderVolume_Start(RenderVolumeArgs* pArgs) = 0;
    virtual Status RenderVolume_DoVolume_Directional(RenderVolumeArgs* pArgs) = 0;
    virtual Status RenderVolume_DoVolume_Spotlight(RenderVolumeArgs* pArgs) = 0;
    virtual Status RenderVolume_DoVolume_Omni(RenderVolumeArgs* pArgs) = 0;
    virtual Status RenderVolume_End(RenderVolumeArgs* pArgs) = 0;
    virtual Status EndAccumulation_Imp(EndAccumulationArgs* pArgs) = 0;
    virtual Status ApplyLighting_Start(ApplyLightingArgs* pArgs) = 0;
    virtual Status ApplyLighting_Resolve(ApplyLightingArgs* pArgs) = 0;
    virtual Status ApplyLighting_TemporalFilter(ApplyLightingArgs* pArgs) = 0;
    virtual Status ApplyLighting_Composite(ApplyLightingArgs* pArgs) = 0;
    virtual Status ApplyLighting_End(ApplyLightingArgs* pArgs) = 0;

    // Size / mode queries
    uint32_t GetOutputBufferWidth() const;
    uint32_t GetOutputBufferHeight() const;
    uint32_t GetOutputViewportWidth() const;
    uint32_t GetOutputViewportHeight() const;
    uint32_t GetOutputSampleCount() const;
    float GetInternalScale() const;
    uint32_t GetInternalBufferWidth() const;
    uint32_t GetInternalBufferHeight() const;
    uint32_t GetInternalViewportWidth() const;
    uint32_t GetInternalViewportHeight() const;
    uint32_t GetInternalSampleCount() const;
    bool IsOutputMSAA() const;
    bool IsInternalMSAA() const;
    FilterMode GetFilterMode() const;
    Vec2 GetJitter() const;
    uint32_t GetCoarseResolution(const VolumeDesc* pVolumeDesc) const;

    // Constant buffer setup
    void SetupCB_PerContext(PerContextCB* cb);
    void SetupCB_PerFrame(const ViewerDesc* pViewerDesc, const MediumDesc* pMediumDesc, PerFrameCB* cb);
    PerVolumeCB* SetupCB_PerVolume(const ShadowMapDesc* pShadowMapDesc, const LightDesc* pLightDesc,
                                   const VolumeDesc* pVolumeDesc, PerVolumeCB* cb);
    PerApplyCB* SetupCB_PerApply(const PostprocessDesc* pPostprocessDesc, PerApplyCB* cb);

    bool isInitialized_;            // +8   per-context constant buffer uploaded
    uint32_t debugFlags_;           // +12  DebugFlags of the current BeginAccumulation
    ContextDesc contextDesc_;       // +16
    ViewerDesc viewerDesc_;         // +40
    uint32_t jitterIndex_;          // +188 index into the 8-step Halton(2,3) sequence
    int32_t lastFrameIndex_;        // +192 history slot of the temporal filter (-1: no history yet)
    int32_t nextFrameIndex_;        // +196 slot written this frame
    Mat44 lastViewProj_;            // +200 previous unjittered view-projection
    Mat44 nextViewProj_;            // +264 current unjittered view-projection
};

} // namespace VolumetricLighting
} // namespace Nv

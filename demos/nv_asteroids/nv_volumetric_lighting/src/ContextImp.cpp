// ContextImp.cpp

#include "ContextImp.h"

#include <math.h>

namespace Nv
{
namespace VolumetricLighting
{

////////////////////////////////////////////////////////////////////////////////
// Construction

// NvVolumetricLighting.d3d11.dll: 0x180003930
ContextImp::ContextImp(const ContextDesc* pContextDesc)
{
    isInitialized_ = false;
    jitterIndex_ = 0;
    lastFrameIndex_ = -1;
    nextFrameIndex_ = 0;
    memcpy(&contextDesc_, pContextDesc, sizeof(ContextDesc));
}

// NvVolumetricLighting.d3d11.dll: 0x1800038D0 (deleting variant 0x1800038F0)
ContextImp::~ContextImp()
{
}

////////////////////////////////////////////////////////////////////////////////
// Stage sequencing

// NvVolumetricLighting.d3d11.dll: 0x1800039D0
Status ContextImp::BeginAccumulation(BeginAccumulationArgs* pArgs)
{
    debugFlags_ = static_cast<uint32_t>(pArgs->debugFlags);
    memcpy(&viewerDesc_, pArgs->pViewerDesc, sizeof(ViewerDesc));
    V_RETURN(BeginAccumulation_Start(pArgs));
    V_RETURN(BeginAccumulation_UpdateMediumLUT(pArgs));
    V_RETURN(BeginAccumulation_CopyDepth(pArgs));
    V_RETURN(BeginAccumulation_End(pArgs));
    return Status::OK;
}

// NvVolumetricLighting.d3d11.dll: 0x180003AB0
Status ContextImp::RenderVolume(RenderVolumeArgs* pArgs)
{
    V_RETURN(RenderVolume_Start(pArgs));
    switch (pArgs->pLightDesc->eType)
    {
    case LightType::DIRECTIONAL:
        V_RETURN(RenderVolume_DoVolume_Directional(pArgs));
        break;
    case LightType::SPOTLIGHT:
        V_RETURN(RenderVolume_DoVolume_Spotlight(pArgs));
        break;
    case LightType::POINT:
        V_RETURN(RenderVolume_DoVolume_Omni(pArgs));
        break;
    default:
        return Status::INVALID_PARAMETER;
    }
    V_RETURN(RenderVolume_End(pArgs));
    return Status::OK;
}

// NvVolumetricLighting.d3d11.dll: 0x180003BC0
Status ContextImp::EndAccumulation(EndAccumulationArgs* pArgs)
{
    V_RETURN(EndAccumulation_Imp(pArgs));
    return Status::OK;
}

// NvVolumetricLighting.d3d11.dll: 0x180003C00
Status ContextImp::ApplyLighting(ApplyLightingArgs* pArgs)
{
    V_RETURN(ApplyLighting_Start(pArgs));
    if (GetFilterMode() == FilterMode::TEMPORAL)
    {
        V_RETURN(ApplyLighting_Resolve(pArgs));
        V_RETURN(ApplyLighting_TemporalFilter(pArgs));
    }
    else if (IsInternalMSAA())
    {
        V_RETURN(ApplyLighting_Resolve(pArgs));
    }
    V_RETURN(ApplyLighting_Composite(pArgs));
    V_RETURN(ApplyLighting_End(pArgs));

    // advance the jitter sequence and swap the temporal history slots
    jitterIndex_ = (jitterIndex_ + 1) % MAX_JITTER_STEPS;
    lastFrameIndex_ = nextFrameIndex_;
    nextFrameIndex_ = (nextFrameIndex_ + 1) % 2;
    return Status::OK;
}

////////////////////////////////////////////////////////////////////////////////
// Size / mode queries

// NvVolumetricLighting.d3d11.dll: 0x180003D90
uint32_t ContextImp::GetOutputBufferWidth() const
{
    return contextDesc_.framebuffer.uWidth;
}

// NvVolumetricLighting.d3d11.dll: 0x180003DA0
uint32_t ContextImp::GetOutputBufferHeight() const
{
    return contextDesc_.framebuffer.uHeight;
}

// NvVolumetricLighting.d3d11.dll: 0x180003DB0
uint32_t ContextImp::GetOutputViewportWidth() const
{
    return viewerDesc_.uViewportWidth;
}

// NvVolumetricLighting.d3d11.dll: 0x180003DD0
uint32_t ContextImp::GetOutputViewportHeight() const
{
    return viewerDesc_.uViewportHeight;
}

// NvVolumetricLighting.d3d11.dll: 0x180003DF0
uint32_t ContextImp::GetOutputSampleCount() const
{
    return contextDesc_.framebuffer.uSamples;
}

// NvVolumetricLighting.d3d11.dll: 0x180003E00
float ContextImp::GetInternalScale() const
{
    switch (contextDesc_.eDownsampleMode)
    {
    case DownsampleMode::HALF:
        return 0.5f;
    case DownsampleMode::QUARTER:
        return 0.25f;
    default:
        return 1.0f;
    }
}

// NvVolumetricLighting.d3d11.dll: 0x180003E50
uint32_t ContextImp::GetInternalBufferWidth() const
{
    switch (contextDesc_.eDownsampleMode)
    {
    case DownsampleMode::HALF:
        return contextDesc_.framebuffer.uWidth >> 1;
    case DownsampleMode::QUARTER:
        return contextDesc_.framebuffer.uWidth >> 2;
    default:
        return contextDesc_.framebuffer.uWidth;
    }
}

// NvVolumetricLighting.d3d11.dll: 0x180003EA0
uint32_t ContextImp::GetInternalBufferHeight() const
{
    switch (contextDesc_.eDownsampleMode)
    {
    case DownsampleMode::HALF:
        return contextDesc_.framebuffer.uHeight >> 1;
    case DownsampleMode::QUARTER:
        return contextDesc_.framebuffer.uHeight >> 2;
    default:
        return contextDesc_.framebuffer.uHeight;
    }
}

// NvVolumetricLighting.d3d11.dll: 0x180003EF0
uint32_t ContextImp::GetInternalViewportWidth() const
{
    switch (contextDesc_.eDownsampleMode)
    {
    case DownsampleMode::HALF:
        return viewerDesc_.uViewportWidth >> 1;
    case DownsampleMode::QUARTER:
        return viewerDesc_.uViewportWidth >> 2;
    default:
        return viewerDesc_.uViewportWidth;
    }
}

// NvVolumetricLighting.d3d11.dll: 0x180003F40
uint32_t ContextImp::GetInternalViewportHeight() const
{
    switch (contextDesc_.eDownsampleMode)
    {
    case DownsampleMode::HALF:
        return viewerDesc_.uViewportHeight >> 1;
    case DownsampleMode::QUARTER:
        return viewerDesc_.uViewportHeight >> 2;
    default:
        return viewerDesc_.uViewportHeight;
    }
}

// NvVolumetricLighting.d3d11.dll: 0x180003F90
uint32_t ContextImp::GetInternalSampleCount() const
{
    switch (contextDesc_.eInternalSampleMode)
    {
    case MultisampleMode::MSAA2:
        return 2;
    case MultisampleMode::MSAA4:
        return 4;
    default:
        return 1;
    }
}

// NvVolumetricLighting.d3d11.dll: 0x180003FD0
bool ContextImp::IsOutputMSAA() const
{
    return GetOutputSampleCount() > 1;
}

// NvVolumetricLighting.d3d11.dll: 0x180004010
bool ContextImp::IsInternalMSAA() const
{
    return GetInternalSampleCount() > 1;
}

// NvVolumetricLighting.d3d11.dll: 0x180004050
FilterMode ContextImp::GetFilterMode() const
{
    return contextDesc_.eFilterMode;
}

// Sub-pixel jitter of the internal buffer (temporal filtering only): Halton(2,3) in [-0.5, 0.5).
// NvVolumetricLighting.d3d11.dll: 0x180004060
Vec2 ContextImp::GetJitter() const
{
    if (GetFilterMode() == FilterMode::TEMPORAL)
    {
        float jitterY = Halton(jitterIndex_, 3) - 0.5f;
        float jitterX = Halton(jitterIndex_, 2);
        return Vec2(jitterX - 0.5f, jitterY);
    }
    else
    {
        return Vec2(0.0f, 0.0f);
    }
}

// Number of patches per side of the light volume mesh: the mesh resolution divided by the maximum
// tessellation factor of the selected hull shader (16 / 32 / 64).
// NvVolumetricLighting.d3d11.dll: 0x1800041C0
uint32_t ContextImp::GetCoarseResolution(const VolumeDesc* pVolumeDesc) const
{
    switch (pVolumeDesc->eTessQuality)
    {
    case TessellationQuality::LOW:
        return pVolumeDesc->uMaxMeshResolution / 16;
    case TessellationQuality::MEDIUM:
        return pVolumeDesc->uMaxMeshResolution / 32;
    default:
        return pVolumeDesc->uMaxMeshResolution / 64;
    }
}

////////////////////////////////////////////////////////////////////////////////
// Constant buffer setup

// NvVolumetricLighting.d3d11.dll: 0x180004230
void ContextImp::SetupCB_PerContext(PerContextCB* cb)
{
    cb->vOutputSize = Vec2((float)GetOutputBufferWidth(), (float)GetOutputBufferHeight());
    cb->vOutputSize_Inv = Vec2(1.0f / cb->vOutputSize.x, 1.0f / cb->vOutputSize.y);
    cb->vBufferSize = Vec2((float)GetInternalBufferWidth(), (float)GetInternalBufferHeight());
    cb->vBufferSize_Inv = Vec2(1.0f / cb->vBufferSize.x, 1.0f / cb->vBufferSize.y);
    cb->fResMultiplier = 1.0f / GetInternalScale();
    cb->uSampleCount = GetInternalSampleCount();
}

// NvVolumetricLighting.d3d11.dll: 0x1800043A0
void ContextImp::SetupCB_PerFrame(const ViewerDesc* pViewerDesc, const MediumDesc* pMediumDesc, PerFrameCB* cb)
{
    cb->mProj = Mat44(pViewerDesc->mProj);
    cb->mViewProj = Mat44(pViewerDesc->mViewProj);
    cb->mViewProj_Inv = Inverse(cb->mViewProj);
    cb->vOutputViewportSize = Vec2((float)GetOutputViewportWidth(), (float)GetOutputViewportHeight());
    cb->vOutputViewportSize_Inv = Vec2(1.0f / cb->vOutputViewportSize.x, 1.0f / cb->vOutputViewportSize.y);
    cb->vViewportSize = Vec2((float)GetInternalViewportWidth(), (float)GetInternalViewportHeight());
    cb->vViewportSize_Inv = Vec2(1.0f / cb->vViewportSize.x, 1.0f / cb->vViewportSize.y);
    cb->vEyePosition = Vec3(pViewerDesc->vEyePosition);
    cb->vJitterOffset = GetJitter();

    // near/far planes recovered from the projection matrix
    float proj33 = cb->mProj(2, 2);
    float proj34 = cb->mProj(2, 3);
    cb->fZNear = -proj34 / proj33;
    cb->fZFar = proj34 / (1.0f - proj33);

    // phase terms; the total scattering starts at a small epsilon to avoid divisions by zero
    Vec3 totalScatter = Vec3(0.000001f, 0.000001f, 0.000001f);
    cb->uNumPhaseTerms = pMediumDesc->uNumPhaseTerms;
    for (uint32_t i = 0; i < pMediumDesc->uNumPhaseTerms; ++i)
    {
        cb->uPhaseFunc[i].value = static_cast<uint32_t>(pMediumDesc->PhaseTerms[i].ePhaseFunc);
        Vec3 density = Vec3(pMediumDesc->PhaseTerms[i].vDensity);
        cb->vPhaseParams[i] = Vec4(density.x, density.y, density.z, pMediumDesc->PhaseTerms[i].fEccentricity);
        totalScatter += density;
    }

    Vec3 absorption = Vec3(pMediumDesc->vAbsorption);
    cb->vScatterPower.x = (float)(1.0 - exp((double)-totalScatter.x));
    cb->vScatterPower.y = (float)(1.0 - exp((double)-totalScatter.y));
    cb->vScatterPower.z = (float)(1.0 - exp((double)-totalScatter.z));
    cb->vSigmaExtinction = totalScatter + absorption;
}

// NvVolumetricLighting.d3d11.dll: 0x1800048E0
PerVolumeCB* ContextImp::SetupCB_PerVolume(const ShadowMapDesc* pShadowMapDesc, const LightDesc* pLightDesc,
                                           const VolumeDesc* pVolumeDesc, PerVolumeCB* cb)
{
    cb->mLightToWorld = Mat44(pLightDesc->mLightToWorld);
    cb->vLightIntensity = Vec3(pLightDesc->vIntensity);
    switch (pLightDesc->eType)
    {
    case LightType::DIRECTIONAL:
        cb->vLightDir = Vec3(pLightDesc->Directional.vDirection);
        break;
    case LightType::SPOTLIGHT:
        cb->vLightDir = Vec3(pLightDesc->Spotlight.vDirection);
        cb->vLightPos = Vec3(pLightDesc->Spotlight.vPosition);
        cb->fLightZNear = pLightDesc->Spotlight.fZNear;
        cb->fLightZFar = pLightDesc->Spotlight.fZFar;
        cb->fLightFalloffAngle = pLightDesc->Spotlight.fFalloff_CosTheta;
        cb->fLightFalloffPower = pLightDesc->Spotlight.fFalloff_Power;
        cb->vAttenuationFactors = *reinterpret_cast<const Vec4*>(pLightDesc->Spotlight.fAttenuationFactors);
        break;
    case LightType::POINT:
        cb->vLightPos = Vec3(pLightDesc->Omni.vPosition);
        cb->fLightZNear = pLightDesc->Omni.fZNear;
        cb->fLightZFar = pLightDesc->Omni.fZFar;
        cb->vAttenuationFactors = *reinterpret_cast<const Vec4*>(pLightDesc->Omni.fAttenuationFactors);
        break;
    default:
        break;
    }
    cb->fDepthBias = pVolumeDesc->fDepthBias;
    cb->uMeshResolution = GetCoarseResolution(pVolumeDesc);

    // size of one coarse grid cell: derived from the diagonal of the light's far plane in world space
    Vec4 vw1 = Transform(cb->mLightToWorld, Vec4(-1.0f, -1.0f, 1.0f, 1.0f));
    Vec4 vw2 = Transform(cb->mLightToWorld, Vec4(1.0f, 1.0f, 1.0f, 1.0f));
    vw1 = vw1 / vw1.w;
    vw2 = vw2 / vw2.w;
    Vec3 vw2_xyz = vw2.xyz();
    float fCrossLength = Length(vw1.xyz() - vw2_xyz);
    cb->fGridSectionSize = sqrtf(0.5f * fCrossLength * fCrossLength) / (float)cb->uMeshResolution;
    cb->fTargetRaySize = pVolumeDesc->fTargetRayResolution;

    // all four shadow map elements are always copied, regardless of uElementCount
    for (int i = 0; i < (int)MAX_SHADOWMAP_ELEMENTS; ++i)
    {
        cb->vElementOffsetAndScale[i].x = (float)pShadowMapDesc->Elements[i].uOffsetX / (float)pShadowMapDesc->uWidth;
        cb->vElementOffsetAndScale[i].y = (float)pShadowMapDesc->Elements[i].uOffsetY / (float)pShadowMapDesc->uHeight;
        cb->vElementOffsetAndScale[i].z = (float)pShadowMapDesc->Elements[i].uWidth / (float)pShadowMapDesc->uWidth;
        cb->vElementOffsetAndScale[i].w = (float)pShadowMapDesc->Elements[i].uHeight / (float)pShadowMapDesc->uHeight;
        cb->mLightProj[i] = Mat44(pShadowMapDesc->Elements[i].mViewProj);
        cb->mLightProj_Inv[i] = Inverse(cb->mLightProj[i]);
        cb->uElementIndex[i].value = pShadowMapDesc->Elements[i].mArrayIndex;
    }
    cb->vShadowMapDim.x = (float)pShadowMapDesc->uWidth;
    cb->vShadowMapDim.y = (float)pShadowMapDesc->uHeight;
    return cb;
}

// NvVolumetricLighting.d3d11.dll: 0x180004F60
PerApplyCB* ContextImp::SetupCB_PerApply(const PostprocessDesc* pPostprocessDesc, PerApplyCB* cb)
{
    if (GetFilterMode() == FilterMode::TEMPORAL)
    {
        cb->fHistoryFactor = (lastFrameIndex_ == -1) ? 0.0f : pPostprocessDesc->fTemporalFactor;
        cb->fFilterThreshold = pPostprocessDesc->fFilterThreshold;
        if (lastFrameIndex_ == -1)
        {
            // first frame: no history, reproject onto itself
            lastViewProj_ = Mat44(pPostprocessDesc->mUnjitteredViewProj);
            lastFrameIndex_ = (nextFrameIndex_ + 1) % 2;
        }
        else
        {
            lastViewProj_ = nextViewProj_;
        }
        nextViewProj_ = Mat44(pPostprocessDesc->mUnjitteredViewProj);
        cb->mHistoryXform = lastViewProj_ * Inverse(nextViewProj_);
    }
    else
    {
        cb->mHistoryXform = Mat44(IdentityTag());
        cb->fHistoryFactor = 0.0f;
        cb->fFilterThreshold = 0.0f;
    }
    cb->vFogLight = Vec3(pPostprocessDesc->vFogLight);
    cb->fMultiScattering = pPostprocessDesc->fMultiscatter;
    return cb;
}

} // namespace VolumetricLighting
} // namespace Nv

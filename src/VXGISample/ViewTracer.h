#ifndef SRC_VXGISAMPLE_VIEWTRACER_H
#define SRC_VXGISAMPLE_VIEWTRACER_H
#include "VXGIIntTypes.h"
#include <nvrhi/core/Foundation.h>
#include <nvrhi/core/AutoPtr.h>
#include <nvrhi/nvrhi.h>
#include <donut/core/salieri.h>
#include <donut/engine/ShaderFactory.h>

namespace donut::engine {
class ShaderFactory;
class BindingCache;
class TextureCache;
}  // namespace donut::engine

namespace vxgi {

class VoxelRenderer;

class ViewTracer : public nvrhi::ObjectImpl<nvrhi::IObject> {
public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ViewTracer)
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IObject)
    NVRHI_END_INTERFACE_TABLE()

   struct TextureHandleVerbose {
       nvrhi::TextureHandle value;
       nvrhi::TextureHandle attachment;
   };

   struct GBufferParameters {
       float4x4 viewProjMatrix;
       float4x4 viewProjMatrixInv;
       float4x4 viewMatrix;
       float4 cameraPosition;
       float4 uvToView;
       float2 gbufferSize;
       float2 gbufferSizeInv;
       float2 viewportOrigin;
       float2 viewportSize;
       float2 viewportSizeInv;
       float2 firstSamplePosition;
       float projectionA;
       float projectionB;
       float depthScale;
       float depthBias;
       float normalScale;
       float normalBias;
       float radiusToScreen;
       float padding;
   };

   struct BuiltinTracingConstants {
       GBufferParameters m_GBuffer;
       GBufferParameters m_PreviousGBufer;
       float4x4 m_ReprojectionMatrix;
       float4 m_vAmbientColor;
       float4 m_vDownsampleScale;
       float4 m_vDebugParams;
       float4 m_fEnvironmentMapTint;
       float4 m_vRefinementGridResolution;
       float4 m_vBackgroundColor;
       int2 m_vPixelToSave;
       int2 m_vRandomOffset;
       float2 m_vGridOrigin;
       float m_fConeFactor;
       float m_fTracingStep;
       float m_fOpacityCorrectionFactor;
       int m_nMaxSamples;
       int m_nNumCones;
       float m_frNumCones;
       float m_fEmittanceScale;
       float m_fEnvironmentMapResolution;
       float m_fMaxEnvironmentMapMipLevel;
       float m_fNormalOffsetFactor;
       float m_fAmbientAttenuationFactor;
       unsigned int m_bFlipOpacityDirections;
       float m_fInitialOffsetBias;
       float m_fInitialOffsetDistanceFactor;
       unsigned int m_bEnableSpecularRandomOffsets;
       int m_nNumDiscontinuityLevels;
       float m_fTemporalReprojectionWeight;
       float m_fTangentJitterScale;
       float m_fDepthdeltaSign;
       float m_fReprojectionDepthWeightScale;
       float m_fReprojectionNormalWeightExponent;
       float m_fInterpolationWeightThreshold;
       unsigned int m_bEnableRefinement;
       float m_fAmbientScale;
       float m_fAmbientBias;
       float m_fAmbientPower;
       float m_fAmbientDistanceDarkening;
       int m_nAltSettingsStencilMask;
       int m_nAltSettingsStencilRefValue;
       float m_fAltInitialOffsetBias;
       float m_fAltInitialOffsetDistanceFactor;
       float m_fAltNormalOffsetFactor;
       float m_fAltTracingStep;
       float m_fSSAO_SurfaceBias;
       float m_fSSAO_RadiusWorld;
       float m_fSSAO_rBackgroundViewDepth;
       float m_fSSAO_CoarseAO;
       float m_fSSAO_PowerExponent;
   };

   struct VisualizeSamplesConstants {
       float4x4 m_ViewProjMatrix;
       float4 m_SmallestVoxelSize;
       float4 m_ClipmapCenter;
       float4 m_ClipmapCenterAtSave;
       float4 m_ToroidalOffset;
       unsigned int m_TotalMipLevels;
       unsigned int m_ColorSelection;
       unsigned int m_OnlyContributingSamples;
       int m_ConeIndexFilter;
       int m_SampleIndexFilter;
       unsigned int m_nStackSize;
       unsigned int m_nStackTextureSize;
       unsigned int m_nPackingStride;
   };

   struct VXGI_ALIGN(8) DownsampleConstants {
       float2 m_vrSourcePixelSize;
       float m_fSourceLod;
   };

   struct SampleData {
       float3 worldSamplePos;
       float mipLevel;
       float3 sampledEmittance;
       float accumulatedOcclusion;
       float3 direction;
       int coneIndex;
       int sampleIndex;
       float sampleT;
   };

   ViewTracer(VoxelRenderer *parent, bool ambientOcclusion);
   ~ViewTracer();

   donut::engine::ShaderMacro GetEmittanceFormatMacroForSampling();

   Status AllocateResources(nvrhi::ICommandList *commandList);
   Status ReleaseResources();

   Status LoadTracingShaders(int sparsity, bool gbufferMSAA);
   void ReleaseTracingShaders();

   void setPixelToSave(nvrhi::ICommandList *commandList, int x, int y);
   float getDiffuseConeAngle(int numCones);
   void FillTracingConstants(BuiltinTracingConstants *builtinConstants, const CommonTracingParameters &params,
                             const ViewTracerInputBuffers *inputBuffers,
                             _In_opt_ const ViewTracerInputBuffers *inputBuffersPreviousFrame,
                             const uint2 &gbufferTextureSize);
   void FillGBufferConstants(GBufferParameters *params, const ViewTracerInputBuffers *inputBuffers,
                             const uint2 &gbufferTextureSize);
   void UpdateConeVectors(nvrhi::ICommandList *commandList, uint numCones, bool enableConeRotation,
                          bool enableRandomConeOffsets, float coneNormalGroupingFactor);

   Status ValidateDownsampledTargets(uint width, uint height, bool useFiltering);
   Status ValidateSpecularTargets(uint width, uint height, bool useFiltering);
   Status ValidateInterpolatedTargets(uint width, uint height);
   Status ValidateTextureSize(TextureHandleVerbose *texture, uint width, uint height, uint arraySize,
                              nvrhi::Format format, bool *invalidated);
   Status SortDiffuseRenderTargetsForTR(nvrhi::ITexture *previousDepthTexture);
   Status SortSpecularRenderTargetsForTR(nvrhi::ITexture *previousDepthTexture);

   Status computeDiffuseChannel(nvrhi::ICommandList *commandList, const DiffuseTracingParameters &params,
                                _Out_ nvrhi::ITexture **outDiffuse,
                                _In_ const ViewTracerInputBuffers *inputBuffers,
                                _In_opt_ const ViewTracerInputBuffers *inputBuffersPreviousFrame);
   Status computeSpecularChannel(nvrhi::ICommandList *commandList, const SpecularTracingParameters &params,
                                 _Out_ nvrhi::ITexture **outSpecular,
                                 _In_ const ViewTracerInputBuffers *inputBuffers,
                                 _In_opt_ const ViewTracerInputBuffers *inputBuffersPreviousFrame);
   Status renderSamplesDebug(nvrhi::ICommandList *commandList, nvrhi::ITexture *destinationTexture, nvrhi::ITexture *destinationDepth, const TracedSamplesParameters &params,
                             _In_ const ViewTracerInputBuffers *inputBuffers);
   Status renderTracerVision(nvrhi::ICommandList *commandList, const TracerVisionParameters &params,
                             nvrhi::ITexture *destinationTexture,
                             _In_ const ViewTracerInputBuffers *inputBuffers);

private:
   VoxelRenderer *m_Parent;
   nvrhi::IDevice *m_Device;
   donut::engine::ShaderFactory *m_ShaderFactory;

   nvrhi::Format m_AvailableDSFormat;

   nvrhi::BufferHandle m_pTracingCB;
   nvrhi::SamplerHandle m_pSamplerLinearBorder;
   nvrhi::SamplerHandle m_EnvironmentMapSampler;
   nvrhi::SamplerHandle m_PointSampler;
   int3 m_PixelToSave = {-1};
   nvrhi::BufferHandle m_SamplePositions;
   nvrhi::BufferHandle m_ConeDirections;
   nvrhi::BufferHandle m_DrawIndirectArgs;
   nvrhi::BufferHandle m_AppendCounters;
   nvrhi::BufferHandle m_VisualizeSamplesCB;
   bool m_ShaderReloadRequired = 0;
   bool m_PreviousFrameGBufferMSAA = 0;
   float4 m_ClipmapCenterAtSampleSave = 0.f;
   bool m_AmbientOcclusionMode = 0;
   float m_SumOfConeDotNormals = 0;
   DiffuseTracingParameters m_PreviousDiffuseTracingParams = {};
   int m_PreviousTracingShaderSparsity = 0;
   nvrhi::ShaderHandle m_pVisualizeSamplesVS;
   nvrhi::ShaderHandle m_pVisualizeSamplesGS;
   nvrhi::ShaderHandle m_pVisualizeTexelsGS;
   nvrhi::ShaderHandle m_pVisualizeSamplesPS;
   nvrhi::ShaderHandle m_pVisualizeTexelsPS;
   nvrhi::ShaderHandle m_pVisualizeConesGS;
   nvrhi::ShaderHandle m_pVisualizeConesPS;
   nvrhi::ShaderHandle m_pTracerVisionPS;
   nvrhi::ShaderHandle m_pTracerVisionDebugPS;
   TextureHandleVerbose m_DownsampledTargets[3];
   TextureHandleVerbose m_DownsampledSmoothedTargets[3];
   TextureHandleVerbose m_SpecularTargets[2];
   TextureHandleVerbose m_FilteredSpecularTarget;
   TextureHandleVerbose m_DeinterleavedDepthTextureArray;
   TextureHandleVerbose m_ScreenSpaceOcclusionTextureArray;
   TextureHandleVerbose m_ScreenSpaceOcclusionBlurTexture;
   int m_DownsampledTargetCount = 0;
   TextureHandleVerbose m_InterpolatedTargets[2];
   TextureHandleVerbose m_RefinementControlTexture;
   TextureHandleVerbose m_RefinementGridTexture;
   TextureHandleVerbose m_RefinementStencilTexture;
   nvrhi::TextureHandle m_RandomsTextureSmall;
   nvrhi::TextureHandle m_RandomsTextureLarge;
   nvrhi::TextureHandle m_ConeDirectionsTexture;
   nvrhi::TextureHandle m_NullTexture;
   nvrhi::TextureHandle m_NullDepthStencilTexture;
   nvrhi::TextureHandle m_NullCubemap;
   nvrhi::ShaderHandle m_pDiffuseTracingPS;
   nvrhi::ShaderHandle m_pDiffuseTracingDebugPS;
   nvrhi::GraphicsPipelineHandle m_pDiffuseTracingRefinePS;
   nvrhi::ComputePipelineHandle m_pInterpolateCoarseCS;
   nvrhi::ComputePipelineHandle m_pInterpolateIlluminationCS;
   nvrhi::ComputePipelineHandle m_pFilterSpecularCS;
   nvrhi::GraphicsPipelineHandle m_pCopyToStencilPS;
   nvrhi::ComputePipelineHandle m_pComputeScreenSpaceOcclusionCS;
   nvrhi::ComputePipelineHandle m_pDeinterleaveDepthCS;
   nvrhi::ComputePipelineHandle m_pBlurScreenSpaceOcclusionCS;
   nvrhi::ShaderHandle m_ReprojectPreviousFramePS;
   nvrhi::ShaderHandle m_GenerateDiscontinuityMapPS;
   nvrhi::ShaderHandle m_DownsampleDiscontinuityMapCS;
   nvrhi::ShaderHandle m_VisualizeDiscontinuityMapPS;
   nvrhi::ShaderHandle m_AllocateTracingPointsCS;
   nvrhi::ShaderHandle m_ComputeTraceDispatchArgumentsCS;
   nvrhi::ShaderHandle m_ClearTracingResultsCS;
   nvrhi::ShaderHandle m_TraceCS;
   nvrhi::ShaderHandle m_SplatVS;
   nvrhi::ShaderHandle m_SplatPS;
   nvrhi::ShaderHandle m_NormalizePS;
   nvrhi::BufferHandle m_DownsampleCB;
   TextureHandleVerbose m_DiscontinuityMipmap;
   TextureHandleVerbose m_DenormalizedDiffuseTargets[2];
   TextureHandleVerbose m_NormalizedDiffuseTarget;
   nvrhi::BufferHandle m_TracingPointsBuffer;
   nvrhi::BufferHandle m_TracingResultsBufferX;
   nvrhi::BufferHandle m_TracingResultsBufferY;
   nvrhi::BufferHandle m_TracingResultsBufferZ;
   nvrhi::SamplerHandle m_DiscontinuitySampler;

   nvrhi::BindingLayoutHandle m_GraphicsBindingLayout;
   nvrhi::BindingLayoutHandle m_GraphicsBindingLayoutDebug;
   nvrhi::BindingLayoutHandle m_ComputeBindingLayout;
   nvrhi::GraphicsPipelineHandle m_DiffuseTracingPSOs[2][2]; // Debug,NumRenderTargets
   nvrhi::FramebufferHandle m_DownsampledSmoothedFb;
   nvrhi::FramebufferHandle m_DownsampledFb;
   nvrhi::FramebufferHandle m_InterpolatedFbs[2];
   nvrhi::FramebufferHandle m_RefinementStencilFb;
   nvrhi::FramebufferHandle m_DiffuseTracingRefineFbs[2];

   nvrhi::GraphicsPipelineHandle m_SpecularTracingPSOs[2]; // Debug
   nvrhi::FramebufferHandle m_SpecularTracingFbs[2];

   nvrhi::BindingLayoutHandle m_pVisualizeBindingLayout;
   nvrhi::GraphicsPipelineHandle m_pVisualizeSamplesPSO;
   nvrhi::GraphicsPipelineHandle m_pVisualizeTexelsPSO;
   nvrhi::GraphicsPipelineHandle m_pVisualizeConesPSO;

   nvrhi::FramebufferHandle m_DebugFramebuffer;

   nvrhi::GraphicsPipelineHandle m_TracerVisionPSOs[2];
   nvrhi::FramebufferHandle m_TracerVisionFramebuffer;
};

}  // namespace vxgi

#endif /* SRC_VXGISAMPLE_VIEWTRACER_H */

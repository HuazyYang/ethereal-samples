#ifndef SRC_VXGISAMPLE_VOXELTEXTURE_H
#define SRC_VXGISAMPLE_VOXELTEXTURE_H
#include "VXGIIntTypes.h"
#include <nvrhi/nvrhi.h>
#include <nvrhi/core/autoptr.h>
#include <donut/engine/ShaderFactory.h>

namespace donut::engine {
class ShaderFactory;
class BindingCache;
class TextureCache;
}

namespace vxgi {

class VoxelRenderer;
struct AllocationMap;

struct VoxelTexture {

    struct ScissorStats {
        uint pixelsRasterized;
        uint pixelsKilledByZViewport;
        uint pixelsKilledByScissorBox;
        uint pixelsKilledByFinerLOD;
        uint voxelsWithNoCoverage;
        uint voxelsKilledByScissorBox;
        uint voxelsKilledByAllocationMap;
    };

    struct VXGI_ALIGN(16) TraceIrradianceBuffer {
        MultiListParams listParams;
        float4 clipmapOrigin;
        float4 irradianceVoxelSize;
        int4 toroidalOffset;
        int4 sourceLevelSize;
        uint irradianceMapSize;
        uint logPageSize;
        uint tracingMaxSamples;
        float tracingStep;
        float tracingOpacityCorrectionFactor;
        float tracingIrradianceScale;
        uint tracingFlipOpacityDirections;
        float tracingConeFactor;
        uint useIrradianceNormalization;
        float irradianceClampValue;
    };

    struct VXGI_ALIGN(16) VoxelizationBuffer {
        float4 gridCenter;
        float4 gridCenterPrevious;
        int4 toroidalOffset;
        box<float, 4> scissorRegionsClipSpace[MAX_STACK_LEVELS];
        int4 textureToAmapTranslation[MAX_STACK_LEVELS];
        float irradianceMapSize;
        uint clipLevelSize;
        uint packingStride;
        uint allocationMapSize;
        uint clipLevelMask;
        uint maxClipLevel;
        int firstLevelToDiscard;
        float discardLower;
        float discardUpper;
        float discardClipSpace;
        uint useCullFunction;
        float emittanceStorageScale;
        uint useIrradianceMap;
        uint use6DOpacity;
        uint useFP32Emittance;
        uint persistentVoxelData;
        uint useInvalidateBitmap;
    };

    struct VXGI_ALIGN(8) VoxelizationMaterialBuffer {
        float4 resolutionFactors[5];
        float noiseScale;
        float noiseBias;
        int twoSided;
        int proportionalEmittance;
        int depthSamples;
        int frontCCW;
        int omnidirectionalLight;
    };

    struct VXGI_ALIGN(16) ListProcessingBuffer {
        MultiListParams listParams;
        int4 toroidalOffsetCurrent;
        int4 toroidalOffsetPrevious;
        int4 translationParamsCurrent;
        int4 translationParamsPrevious;
        uint4 textureSize;
        float4 clipmapAnchorOffset;
        float4 rEmittanceTextureSize;
        uint levelSizeCurrent;
        uint levelSizePrevious;
        uint allocationLodBias;
        uint levelToProcess;
        uint stackSize;
        uint numPagedLevels;
        uint packingStride;
        uint opacityPackingOffsetCurrent;
        uint opacityPackingOffsetPrevious;
        uint emittancePackingOffsetCurrent;
        uint emittancePackingOffsetPrevious;
        uint useEmittanceInterpolation;
        uint persistentVoxelData;
        float coverageBitCountMultiplier;
    };

    VoxelRenderer *m_Parent;
    nvrhi::IDevice *m_Device;
    donut::engine::ShaderFactory *m_ShaderFactory;
    AllocationMap *m_AllocationMap;

    uint3 m_OpacityTextureSize = {};
    uint3 m_EmittanceTextureSize = {};
    uint3 m_BackupTextureSize = {};
    std::vector<nvrhi::BufferHandle> m_ListProcessingBuffers;
  
    nvrhi::TextureHandle m_TextureCoverage_Pos;
    nvrhi::TextureHandle m_TextureCoverage_Neg;
    nvrhi::TextureHandle m_TextureBackupOpacity_Pos;
    nvrhi::TextureHandle m_TextureBackupOpacity_Neg;
    std::vector<nvrhi::TextureHandle> m_TextureEmittanceEven;
    std::vector<nvrhi::TextureHandle> m_TextureEmittanceOdd;
    std::vector<uint> m_OpacityPackingOffsets;
    std::vector<uint> m_EmittancePackingOffsets;
    nvrhi::TextureHandle m_IrradianceTexture;
    nvrhi::SamplerHandle m_IrradianceSampler;
    nvrhi::BufferHandle m_IrradianceNormalizationBuffer;

    float m_PreviousIrradianceScale = {};

    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BindingLayoutHandle m_DebugBindingLayout;
    nvrhi::SamplerHandle m_LinearWrapSampler;

    nvrhi::ComputePipelineHandle m_DownsampleCoverageCS;
    nvrhi::ComputePipelineHandle m_GenerateOpacityMipmapCS;
    nvrhi::ComputePipelineHandle m_WrapOpacityClipmapCS;
    nvrhi::ComputePipelineHandle m_WrapOpacityMipmapCS;
    nvrhi::ComputePipelineHandle m_GenerateEmittanceMipmapCS;
    nvrhi::ComputePipelineHandle m_WrapEmittanceClipmapCS;
    nvrhi::ComputePipelineHandle m_WrapEmittanceMipmapCS;
    nvrhi::ComputePipelineHandle m_TraceIrradianceMapCS;
    nvrhi::ComputePipelineHandle m_ClearIrradianceMapCS;
    nvrhi::ComputePipelineHandle m_NormalizeIrradianceScaleCS;

    nvrhi::BufferHandle m_VoxelizationBuffer;
    nvrhi::BufferHandle m_VoxelizationMaterialBuffer;
    nvrhi::BufferHandle m_TraceIrradianceBuffer;

    nvrhi::BufferHandle m_ScissorStatsBuffer;

    bool m_EnableStatistics = {};

    ScissorStats m_ScissorStatsData = {};
    nvrhi::BufferHandle m_CoverageMasksBuffer;
    nvrhi::TextureHandle m_ProjectedCoverageTexture;
    nvrhi::FramebufferHandle m_ProjectedCoverageFramebuffer;

    std::vector<nvrhi::ComputePipelineHandle> m_ClearOpacityPagesCS;
    std::vector<nvrhi::ComputePipelineHandle> m_RestoreOpacityPagesCS;
    std::vector<nvrhi::ComputePipelineHandle> m_ConvertCoverageToOpacityCS;
    std::vector<nvrhi::ComputePipelineHandle> m_DownsampleOpacityPagesCS;
    std::vector<nvrhi::ComputePipelineHandle> m_ClearEmittancePagesCS;
    std::vector<nvrhi::ComputePipelineHandle> m_DownsampleEmittancePagesCS;
    std::vector<nvrhi::ComputePipelineHandle> m_ConvertEmissivePagesCS;

    nvrhi::ShaderHandle m_OpacityRaycastPS;
    nvrhi::ShaderHandle m_CoverageRaycastPS;
    nvrhi::ShaderHandle m_EmittanceRaycastPS;
    nvrhi::ShaderHandle m_IrradianceRaycastPS;
    nvrhi::GraphicsPipelineHandle m_OpacityRaycastPSO;
    nvrhi::GraphicsPipelineHandle m_CoverageRaycastPSO;
    nvrhi::GraphicsPipelineHandle m_EmittanceRaycastPSO;
    nvrhi::GraphicsPipelineHandle m_IrradianceRaycastPSO;

    nvrhi::BufferHandle m_DebugBuffer;

    uint m_VoxelizationClipLevelMask = {};
    MaterialSamplingRate m_VoxelizationMaterialSamplingRate = (MaterialSamplingRate)-1;
    box<float, 4> m_VoxelizationScissorRegionsClipSpace[MAX_STACK_LEVELS] = {};
    int4 m_VoxelizationTextureToAmapTranslation[MAX_STACK_LEVELS] = {};

    nvrhi::MonoPtr<donut::engine::BindingCache> m_BindingCache;

    nvrhi::BindingLayoutHandle m_TracingBindingLayout;
    nvrhi::BindingSetHandle m_TracingBindingSet;

    nvrhi::BindingLayoutHandle m_NormalizationIrradianceScaleBindingLayout;

    VoxelTexture(VoxelRenderer *renderer);
    ~VoxelTexture();

    nvrhi::IBindingLayout* GetTracingBindingLayout();
    nvrhi::IBindingSet *GetBindingSetForIrradianceMapTracing();

    nvrhi::IFramebuffer *GetProjectedCoverageFramebuffer();

    donut::engine::ShaderMacro GetEmittanceFormatMacroForVoxelization();
    donut::engine::ShaderMacro GetEmittanceFormatMacroTypedLoad();
    donut::engine::ShaderMacro GetEmittanceFormatMacroForSampling();
    donut::engine::ShaderMacro GetEmittanceFormatMacro();
    donut::engine::ShaderMacro GetEmittanceFormatForAO();

    nvrhi::Format GetEmittanceUAVFormat();

    uint GetEmittanceChannelCount();
    float GetEmittanceStorageScale();
    nvrhi::ITexture *GetEmittanceTexture(int channel, bool isOdd);
    uint3 GetEmittanceTextureSize();
    OpacityDirections GetOpacityDirectionCount();
    nvrhi::ITexture *GetOpacityTexture(bool negative);
    uint3 GetOpacityTextureSize();
    uint GetPackingStride();
    int GetResolutionFactor(MaterialSamplingRate materialSamplingRate, uint clipmapLevel);
    int GetLodPackingOffset(uint level, bool isEmittance);

    uint GetLevelSize(uint level);

    Status AllocateResources(nvrhi::ICommandList *commandList);

    void ProcessOpacity(nvrhi::ICommandList *commandList);
    void ClearOpacityPages(nvrhi::ICommandList *commandList);
    void RestoreOpacityPages(nvrhi::ICommandList *commandList);
    void ClearEmittancePages(nvrhi::ICommandList *commandList);
    void ConvertEmissivePages(nvrhi::ICommandList *commandList);
    void WrapOpacity(nvrhi::ICommandList *commandList);
    void GenerateOpacityMipmaps(nvrhi::ICommandList *commandList);
    void GenerateEmittanceMipmaps(nvrhi::ICommandList *commandList);
    void DownsampleOpacityPages(nvrhi::ICommandList *commandList);
    void DownsampleEmittancePages(nvrhi::ICommandList *commandList);
    void ConvertCoverageToOpacity(nvrhi::ICommandList *commandList);
    void TraceIrradianceMap(nvrhi::ICommandList *commandList, const IndirectIrradianceMapTracingParameters *traceParams);
    void FillVoxelizationBuffer(nvrhi::ICommandList *commandList,
                                const MaterialInfo *material, bool isEmittance,
                                float4 gridCenterPreviousFrame);
    void prepareVoxelizationRenderState(const box3 &scissor, bool isEmittance,
                                        MaterialSamplingRate materialSamplingRate,
                                        uint numMaxViewports,
                                        uint *numViewports,
                                        nvrhi::Viewport *viewports,
                                        nvrhi::Rect *scissorRects
                                    );
    void InvalidateVoxelizationRenderState();
    void ClearStatistics(nvrhi::ICommandList *commandList);
    void FillListProcessingBuffers(nvrhi::ICommandList *commandList);

    Status getVoxelizationState(nvrhi::ICommandList *commandList, bool isEmittance, const MaterialInfo &materialInfo,
                                const float4 &gridCenterPreviousFrame,
                                nvrhi::IBindingLayout *bindingLayout,
                                nvrhi::IBindingSet **bindingSet);

    void updateVoxelizationMaterialParameters(
        nvrhi::ICommandList *commandList,
        const MaterialInfo &material
    );

    donut::engine::BindingCache *GetBindingCache();

    void RenderDebugOpacity(nvrhi::ICommandList *commandList, nvrhi::GraphicsState &state,
                            const float4x4 &viewProjMatrix, float nearClipZ, float farClipZ,
                            const float3 &cameraPos, uint level, uint voxelToSkip, bool renderAllLevels,
                            float targetOpacity);

    void RenderDebugEmittance(nvrhi::ICommandList *commandList, nvrhi::GraphicsState &state,
                              const float4x4 &viewProjMatrix, float nearClipZ, float farClipZ,
                              const float3 &cameraPos, uint level, uint voxelToSkip, bool renderAllLevels,
                              float targetOpacity);

    void RenderDebugIrradiance(nvrhi::ICommandList *commandList, nvrhi::GraphicsState &state,
                               const float4x4 &viewProjMatrix, float nearClipZ, float farClipZ,
                               const float3 &cameraPos, uint voxelToSkip, float targetOpacity);
 private:
    const DerivedVoxelizationParameters *GetVoxelizationParameters();
};

}


#endif /* SRC_VXGISAMPLE_VOXELTEXTURE_H */

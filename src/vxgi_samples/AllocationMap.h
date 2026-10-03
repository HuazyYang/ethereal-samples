#ifndef ALLOCATIONMAP_H
#define ALLOCATIONMAP_H
#include "VXGIIntTypes.h"
#include <vector>
#include <nvrhi/nvrhi.h>
#include <nvrhi/core/autoptr.h>

namespace donut::engine {
class ShaderFactory;
struct ShaderMacro;
class BindingCache;
}

namespace vxgi {

class VoxelRenderer;

struct MultiPageList {
    nvrhi::BufferHandle buffer;
    uint bufferSize;
    uint logPageSize0;
    std::vector<uint> pageCounts;
    std::vector<uint> offsets;

    MultiListParams getListParams(uint level);
    uint getNumLevels();
    size_t getCounterOffset(uint level);
    size_t getDispatchArgumentsOffset(uint level);
};

struct AllocationMap {
    struct VXGI_ALIGN(16) RaycastCB {
        float4 cameraPos;
        float4 gridBounds;
        float4x4 viewProjMatrix;
        float4x4 viewProjMatrixInv;
        int4 toroidalOffset;
        uint textureSize;
        uint opacityPackingOffset;
        uint emittancePackingOffset;
        uint mipOffset;
        uint voxelsToSkip;
        uint allocationMapBit;
        float scale;
        float farClipZ;
        float viewportMinZ;
        float viewportMaxZ;
        float targetOpacity;
        uint skipUpperLevels;
        uint use6DOpacity;
    };

    struct VXGI_ALIGN(16) InvalidateCB {
        MultiListParams filteredPageListParams[MAX_PAGED_LEVELS];
        MultiListParams irradianceMapPageListParams;
        uint4 gridParams[MAX_PAGED_LEVELS];
        uint4 irradianceMapGridParams;
        int4 toroidalOffset;
        int4 toroidalOffsetPrevious;
        float4 clipmapOrigin;
        uint textureSize;
        uint numberOfRegions;
        uint maxClipLevel;
        uint numberOfFrusta;
        uint numPagedLevels;
        uint opacityDownsampleAllPresentPages;
        uint opacityVoxelizeAllLevels;
        uint storeEmittanceInFP16;
        uint emittanceVoxelizeAllLevels;
        uint writeIrradianceMapPages;
    };

    struct RegionToRasterize {
        float4 xyBounds;
        uint4 zBits;
    };

    VoxelRenderer *m_Parent;
    nvrhi::IDevice *m_Device;
    donut::engine::ShaderFactory *m_ShaderFactory;

    MultiPageList m_OpacityPagesToClear;
    MultiPageList m_OpacityPagesToVoxelize;
    MultiPageList m_OpacityPagesToDownsample;
    MultiPageList m_OpacityPagesToRestore;
    MultiPageList m_EmittancePagesToDownsample;
    MultiPageList m_EmittancePagesToClear;
    MultiPageList m_PagesWithEmissiveMaterialsToVoxelize;
    MultiPageList m_IrradiancePagesToProcess[2] = {};

    nvrhi::TextureHandle m_AllocationMap;

    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BindingLayoutHandle m_DebugBindingLayout;

    nvrhi::ShaderHandle m_FullScreenQuadVS;

    nvrhi::ComputePipelineHandle m_InvalidateRegionCS;
    nvrhi::ComputePipelineHandle m_DilateEmissivePagesCS;

    nvrhi::BufferHandle m_InvalidateCB;
    nvrhi::BufferHandle m_InvalidateBuffer;
    nvrhi::BufferHandle m_InvalidateFrustaBuffer;
    nvrhi::BufferHandle m_ListParametersClearBuffer;
    nvrhi::ComputePipelineHandle m_GeneratePageListsCS;
    nvrhi::ComputePipelineHandle m_ComputeDispatchArgumentsFilteredCS;
    nvrhi::ComputePipelineHandle m_ComputeIrradianceDispatchArgumentsCS;
    int m_InvalidateBufferSize = {};

    nvrhi::GraphicsPipelineHandle m_RasterizeInvalidateRegionsPS;
    nvrhi::BindingLayoutHandle m_RasterizeInvalidateRegionsBindingLayout;
    nvrhi::FramebufferHandle m_EmptyFramebuffer;

    nvrhi::TextureHandle m_InvalidateBitmap;
    nvrhi::BufferHandle m_DebugBuffer;
    nvrhi::ShaderHandle m_VoxelizeDebugPS;
    nvrhi::GraphicsPipelineHandle m_VoxelizeDebugPSO;

    nvrhi::MonoPtr<donut::engine::BindingCache> m_BindingCache;
    nvrhi::BindingSetHandle m_RasterizeInvalidateRegionsBindingSet;
    nvrhi::BindingSetHandle m_InvalidateRegionsBindingSet;
    nvrhi::BindingSetHandle m_DilateEmissivePagesBindingSet;
    nvrhi::BindingSetHandle m_GeneratePageListsBindingSets[2];

    std::vector<ibox3> m_RegionsInVoxel;
    std::vector<ibox3> m_RegionsInVoxel2;
    std::vector<int> m_RegionVolumes;
    std::vector<int> m_RegionGains;

    bool m_AlternatingFrameIndex = {};

    AllocationMap(VoxelRenderer *parent);
    ~AllocationMap();

    const DerivedVoxelizationParameters *GetVoxelizationParameters();

    nvrhi::ITexture *GetTexture();

    Status AllocateResources();

    Status CreatePageProcessingCS(const char *sourceFile, bool allPagedLevels,
                                  const donut::engine::ShaderMacro *macros, uint numMacros,
                                  std::vector<nvrhi::ShaderHandle> &shaders);

    void SnapAndInvalidateRegions(nvrhi::ICommandList *comandList,
                                  const std::vector<box3> &regions,
                                  const std::vector<frustum> &frustra,
                                  /*_Out_*/ std::vector<box3> &snappedRegions,
                                  bool anchorMoved);

    void RenderDebug(nvrhi::ICommandList *commandList, nvrhi::GraphicsState &state, const float4x4 &viewProjMatrix,
                     float nearClipZ, float farClipZ, const float3 &cameraPos, uint bitToDisplay,
                     uint voxelToSkip, float targetOpacity);

    void SetPerGpuAlternatingFrameIndex(bool index);

    nvrhi::ITexture *GetInvalidateBitmap();

    void DilateEmissivePages(nvrhi::ICommandList *commandList);
    void GeneratePageLists(nvrhi::ICommandList *commandList);

    nvrhi::IBindingSet *GetIrradiancePageListBindingSet(bool previousFrame);
    MultiPageList *GetIrradiancePageList(bool previousFrame);

 private:
    void OptimizeRegion(const std::vector<ibox3> *inRegion, std::vector<ibox3> *outRegion);

    uint GetPageProcessingGroupSize(uint level);

    Status createMultiPageList(const std::vector<uint> &pageCounts, uint logPageSize0,
                               MultiPageList *list);

    void clearMultiPageList(nvrhi::ICommandList *commandList, MultiPageList *list);

    void ComputeDispatchArguments(nvrhi::ICommandList *commandList, MultiPageList *list, bool irradianceMap);
};


}

#endif /* ALLOCATIONMAP_H */

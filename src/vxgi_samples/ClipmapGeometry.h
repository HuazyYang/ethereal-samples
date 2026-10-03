#ifndef CLIPMAPGEOMETRY_H
#define CLIPMAPGEOMETRY_H
#include "VXGIIntTypes.h"
#include <vector>

namespace vxgi {

struct ClipmapGeometry {
    float3 m_Anchor = {0.f};
    float3 m_Center = {0.f};

    int3 m_AllocationMapToroidalOffset = {0};
    int3 m_AllocationMapToroidalOffsetWrapped = {0};
    box3 m_WorldRegion = box3::empty();
    float m_WorldSize = 0.f;
    float m_PageSize = 0.f;
    std::vector<float> m_LevelSizes = {0.f};
    std::vector<float> m_VoxelSizes = {0.f};
    std::vector<box3> m_LevelRegions = {};
    std::vector<int3> m_ClipmapToroidalOffsets = {};
    std::vector<int3> m_ClipmapToroidalOffsetsDebug = {};

    static float3 calculateClipmapCenter(float3 anchor, float giRange,
                                  const DerivedVoxelizationParameters &params);

    void resize(uint totalLevels);

    void update(float3 anchor, float giRange, const DerivedVoxelizationParameters &params);

    float getNearestLevelBoundary(uint level);
};

}

#endif /* CLIPMAPGEOMETRY_H */

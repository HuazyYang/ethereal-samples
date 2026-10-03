#include "ClipmapGeometry.h"

namespace vxgi {

float3 ClipmapGeometry::calculateClipmapCenter(
    float3 anchor, float giRange, const DerivedVoxelizationParameters& params) {
    float3 pageSize = { 2.f * giRange * float(1 << (params.stackLevels - 1)) / params.allocationMapSize };
    float3 clipmapCenter = dm::vfloor(anchor / pageSize + 0.5f) * pageSize;

    return clipmapCenter;
}

void ClipmapGeometry::resize(uint totalLevels) {
    m_LevelSizes.resize(totalLevels);
    m_VoxelSizes.resize(totalLevels);
    m_LevelRegions.resize(totalLevels);
    m_ClipmapToroidalOffsets.resize(totalLevels);
    m_ClipmapToroidalOffsetsDebug.resize(totalLevels);
}

void ClipmapGeometry::update(float3 anchor, float giRange,
                             const DerivedVoxelizationParameters& params) {
    m_Anchor = anchor;
    m_Center = calculateClipmapCenter(anchor, giRange, params);

    float halfLevelSize = giRange;
    float voxelSize = (2.f * giRange) / (float)params.mapSize;

    for (uint level = 0; level < params.totalLevels; ++level) {
        m_LevelSizes[level] = halfLevelSize * 2.f;
        m_VoxelSizes[level] = voxelSize;

        m_LevelRegions[level] = {m_Center - halfLevelSize, m_Center + halfLevelSize};

        if (level < params.stackLevels - 1)
            halfLevelSize = halfLevelSize * 2.f;

        voxelSize = voxelSize * 2.f;
    }

    m_PageSize = m_VoxelSizes[params.allocationMapLod];
    m_WorldRegion = m_LevelRegions[params.allocationMapLod];
    m_WorldSize = m_LevelSizes[params.allocationMapLod];
    m_AllocationMapToroidalOffset = int3(m_WorldRegion.lower() / m_PageSize);

    m_AllocationMapToroidalOffsetWrapped.x =
        (params.allocationMapSize - 1) & m_AllocationMapToroidalOffset.x;
    m_AllocationMapToroidalOffsetWrapped.y =
        (params.allocationMapSize - 1) & m_AllocationMapToroidalOffset.y;
    m_AllocationMapToroidalOffsetWrapped.z =
        (params.allocationMapSize - 1) & m_AllocationMapToroidalOffset.z;

    for (uint i = 0; i < params.totalLevels; ++i) {
        int shift = std::max<int>(params.pageLevels - i - 1, 0);
        m_ClipmapToroidalOffsets[i] = m_AllocationMapToroidalOffsetWrapped * (1 << shift);

        int allocationMapLodBias = params.allocationMapLodBias;
        allocationMapLodBias =
            std::max<int>(params.pageLevels - i - 1, allocationMapLodBias);
        m_ClipmapToroidalOffsetsDebug[i] =
            m_AllocationMapToroidalOffsetWrapped * (1 << allocationMapLodBias);
    }
}

float ClipmapGeometry::getNearestLevelBoundary(uint level) {
    return (m_LevelSizes[level] - m_PageSize) / 2.f;
}

}  // namespace vxgi

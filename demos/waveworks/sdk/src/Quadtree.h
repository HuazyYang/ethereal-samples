#pragma once

#include <cstdint>
#include <vector>
#include <array>
#include "Types.h"
#include <waveworks/GFSDK_WaveWorks.h>

struct QuadNode
{
    float x; // offset 0
    float y; // offset 4
    float size; // offset 8
    int child0; // offset 12
    int child1; // offset 16
    int child2; // offset 20
    int child3; // offset 24
    int depth; // offset 28
    float distanceSquared; // offset 32

    bool isLeaf() const
    {
        return child0 < 0 && child1 < 0 && child2 < 0 && child3 < 0;
    }
};

struct Quadtree
{
public:
    explicit Quadtree(const GFSDK_WaveWorks_Quadtree_Params& params);
    ~Quadtree();

    void reinit(const GFSDK_WaveWorks_Quadtree_Params& params);

    bool getGeometryData(
        uint32_t* pNumVertices,
        float2** ppVertexPositions,
        uint32_t* pNumIndices,
        uint32_t** ppIndices) const;

    bool getNodes(
        const float4x4& viewMatrix,
        const float4x4& projMatrix,
        const float2& viewportSize,
        float cullMargin,
        uint32_t* pNumNodes,
        GFSDK_WaveWorks_Quadtree_NodeRenderingProperties** ppNodes);

    void getStats(GFSDK_WaveWorks_Quadtree_Stats& stats) const;

private:
    struct NodeTopology
    {
        uint32_t indexOffset; // offset 0
        uint32_t indexCount; // offset 4
        uint32_t topologyId; // offset 8
    };

    void initGeometry();
    int64_t generateInnerIndices(uint32_t xPadding0, int xPadding1, uint32_t yPadding0, uint32_t yPadding1);
    int64_t generateSkirtIndices(int xPadding0, int xPadding1, int yPadding0, int yPadding1);

    bool isNodeVisible(const QuadNode& node) const;
    int64_t buildNodeList(QuadNode* node);
    void calculateDistanceToCameraSquared(QuadNode& node) const;
    float estimateGridCellEdgeLength(const QuadNode& node) const;

    void rebuildSearchAccelerator();
    bool refineNodeList();

    int64_t findIndexOfLeafContainingPoint(const float2& point) const;
    int64_t findIndexOfNodeOfThisDepthContainingPoint(const float2& point, uint32_t depth) const;

    int64_t findBottomNeighborWithSameDepth(const QuadNode& node) const;
    int64_t findLeftNeighborWithSameDepth(const QuadNode& node) const;
    int64_t findRightNeighborWithSameDepth(const QuadNode& node) const;
    int64_t findTopNeighborWithSameDepth(const QuadNode& node) const;

    const NodeTopology* selectNodeTopology(const QuadNode& node) const;
    static bool compareQuadNodesForFrontToBackSorting(const QuadNode& a, const QuadNode& b);

    uint32_t m_cellCount; // offset 0
    float m_minPatchLength; // offset 4
    uint32_t m_maxLODNumber; // offset 8
    float m_maxEdgeLength; // offset 12
    float m_meanSeaLevel; // offset 16
    bool m_generateDiamondPattern; // offset 20
    float m_geomorphingDegree; // offset 24

    float m_cameraX; // offset 28
    float m_cameraY; // offset 32
    float m_cameraZ; // offset 36
    float m_one; // offset 40

    float4x4 m_viewMatrix; // offset 44
    float4x4 m_projMatrix; // offset 108
    float4x4 m_viewProjMatrix; // offset 172

    float m_viewportWidth; // offset 236
    float m_viewportHeight; // offset 240
    float m_scaleFactor; // offset 244
    float m_timeValue; // offset 248

    std::array<NodeTopology, 16> m_nodeTopologies; // offset 252
    std::vector<float2> m_gridPoints; // offset 448
    std::vector<uint32_t> m_indexBuffer; // offset 472
    std::vector<QuadNode> m_rootNodes; // offset 496
    std::vector<QuadNode> m_nodeList; // offset 520
    std::vector<QuadNode> m_sortedNodeList; // offset 544
    std::vector<std::vector<uint32_t>> m_nodesByDepth; // offset 568
    std::vector<GFSDK_WaveWorks_Quadtree_NodeRenderingProperties> m_renderProps; // offset 592

    uint32_t m_maxDepth; // offset 616
    GFSDK_WaveWorks_Quadtree_Stats m_stats; // offset 620
};

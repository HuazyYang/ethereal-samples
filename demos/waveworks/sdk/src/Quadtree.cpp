#include "Quadtree.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

static bool pointInsideNode(const QuadNode& node, const float2& point)
{
    return point.x >= node.x
        && point.x <= node.x + node.size
        && point.y >= node.y
        && point.y <= node.y + node.size;
}

Quadtree::Quadtree(const GFSDK_WaveWorks_Quadtree_Params& params)
    : m_cellCount(0) // pseudo: bytes 0..27 are zeroed before reinit's "geometry changed" test
    , m_minPatchLength(0.f)
    , m_maxLODNumber(0)
    , m_maxEdgeLength(0.f)
    , m_meanSeaLevel(0.f)
    , m_generateDiamondPattern(false)
    , m_geomorphingDegree(0.f)
    , m_nodeTopologies{}
    , m_maxDepth(0)
{
    m_stats = {};
    reinit(params);
}

Quadtree::~Quadtree() = default;

void Quadtree::reinit(const GFSDK_WaveWorks_Quadtree_Params& params)
{
    bool geometryNeedsRebuild =
        params.cell_count != m_cellCount ||
        params.generate_diamond_pattern != m_generateDiamondPattern;

    m_cellCount = (params.cell_count + 15u) & ~15u;
    if (m_cellCount < 16u)
        m_cellCount = 16u;
    else if (m_cellCount > 128u)
        m_cellCount = 128u;

    m_minPatchLength = params.min_patch_length;
    m_maxLODNumber = params.max_LOD_number;
    m_maxEdgeLength = params.max_edge_length;
    m_meanSeaLevel = params.mean_sea_level;
    m_generateDiamondPattern = params.generate_diamond_pattern;
    m_geomorphingDegree = params.geomorphing_degree;

    if (m_maxEdgeLength < 2.0f)
        m_maxEdgeLength = 2.0f;
    else if (m_maxEdgeLength > 100.0f)
        m_maxEdgeLength = 100.0f;

    if (geometryNeedsRebuild)
    {
        m_gridPoints.clear();
        m_indexBuffer.clear();
        m_renderProps.clear();
        m_maxDepth = 0;
        initGeometry();
    }
}

bool Quadtree::getGeometryData(
    uint* pNumVertices,
    float2** ppVertexPositions,
    uint* pNumIndices,
    uint** ppIndices) const
{
    if (!m_gridPoints.empty() && !m_indexBuffer.empty())
    {
        *pNumVertices = static_cast<uint>(m_gridPoints.size());
        *ppVertexPositions = const_cast<float2*>(m_gridPoints.data());
        *pNumIndices = static_cast<uint>(m_indexBuffer.size());
        *ppIndices = const_cast<uint*>(m_indexBuffer.data());
        return true;
    }

    *pNumVertices = 0;
    *pNumIndices = 0;
    *ppVertexPositions = nullptr;
    *ppIndices = nullptr;
    return false;
}

int64_t Quadtree::generateInnerIndices(uint xPadding0, int xPadding1, uint yPadding0, uint yPadding1)
{
    int64_t count = 0;
    const uint gridCount = m_cellCount;
    const uint rowStride = gridCount + 1;
    const uint limitY = gridCount - yPadding1;
    const uint limitX = gridCount - xPadding1;

    for (uint y = yPadding0; y < limitY; ++y)
    {
        for (uint x = xPadding0; x < limitX; ++x)
        {
            uint y1;
            if (((x + y) & 1u) != 0u && m_generateDiamondPattern)
            {
                m_indexBuffer.push_back(x + y * rowStride + 1);
                m_indexBuffer.push_back(x + (y + 1) * rowStride + 1);
                m_indexBuffer.push_back(x + (y + 1) * rowStride);
                m_indexBuffer.push_back(x + y * rowStride);
                y1 = y;
            }
            else
            {
                m_indexBuffer.push_back(x + y * rowStride);
                m_indexBuffer.push_back(x + y * rowStride + 1);
                m_indexBuffer.push_back(x + (y + 1) * rowStride + 1);
                m_indexBuffer.push_back(x + y * rowStride);
                y1 = y + 1;
            }

            m_indexBuffer.push_back(x + y1 * rowStride + 1);
            m_indexBuffer.push_back(x + (y + 1) * rowStride);
            count += 6;
        }
    }

    return count;
}

int64_t Quadtree::generateSkirtIndices(int xPadding0, int xPadding1, int yPadding0, int yPadding1)
{
    int64_t count = 0;
    const uint gridCount = m_cellCount;
    const uint rowStride = gridCount + 1;

    if(!gridCount)
        return 0;

    if (yPadding0)
    {
        for (int x = 0; x < static_cast<int>(gridCount); x += 2)
        {
            m_indexBuffer.push_back(static_cast<uint>(x));
            m_indexBuffer.push_back(static_cast<uint>(x + 2));
            m_indexBuffer.push_back(static_cast<uint>(x + gridCount + 2));
            count += 3;

            if (x != 0 || yPadding0 != xPadding0)
            {
                m_indexBuffer.push_back(static_cast<uint>(x));
                m_indexBuffer.push_back(static_cast<uint>(x + gridCount + 2));
                m_indexBuffer.push_back(static_cast<uint>(x + gridCount + 1));
                count += 3;
            }

            if (x != static_cast<int>(gridCount) - 2 || yPadding0 != xPadding1)
            {
                m_indexBuffer.push_back(static_cast<uint>(x + 2));
                m_indexBuffer.push_back(static_cast<uint>(x + gridCount + 3));
                m_indexBuffer.push_back(static_cast<uint>(x + gridCount + 2));
                count += 3;
            }
        }
    }

    if (xPadding1)
    {
        for (int y = 0; y < static_cast<int>(gridCount); y += 2)
        {
            m_indexBuffer.push_back(static_cast<uint>(gridCount + y * rowStride));
            m_indexBuffer.push_back(static_cast<uint>(gridCount + (y + 2) * rowStride));
            m_indexBuffer.push_back(static_cast<uint>(gridCount - 1 + (y + 1) * rowStride));
            count += 3;

            if (y != 0 || xPadding1 != yPadding0)
            {
                m_indexBuffer.push_back(static_cast<uint>(gridCount + y * rowStride));
                m_indexBuffer.push_back(static_cast<uint>(gridCount - 1 + (y + 1) * rowStride));
                m_indexBuffer.push_back(static_cast<uint>(gridCount - 1 + y * rowStride));
                count += 3;
            }

            if (y != static_cast<int>(gridCount) - 2 || xPadding1 != yPadding1)
            {
                m_indexBuffer.push_back(static_cast<uint>(gridCount + (y + 2) * rowStride));
                m_indexBuffer.push_back(static_cast<uint>(gridCount - 1 + (y + 2) * rowStride));
                m_indexBuffer.push_back(static_cast<uint>(gridCount - 1 + (y + 1) * rowStride));
                count += 3;
            }
        }
    }

    if (yPadding1)
    {
        for (int x = 0; x < static_cast<int>(gridCount); x += 2)
        {
            m_indexBuffer.push_back(static_cast<uint>(x + gridCount * rowStride));
            m_indexBuffer.push_back(static_cast<uint>(x + (gridCount - 1) * rowStride + 1));
            m_indexBuffer.push_back(static_cast<uint>(x + gridCount * rowStride + 2));
            count += 3;

            if (x != 0 || yPadding1 != xPadding0)
            {
                m_indexBuffer.push_back(static_cast<uint>(x + gridCount * rowStride));
                m_indexBuffer.push_back(static_cast<uint>(x + (gridCount - 1) * rowStride));
                m_indexBuffer.push_back(static_cast<uint>(x + (gridCount - 1) * rowStride + 1));
                count += 3;
            }

            if (x != static_cast<int>(gridCount) - 2 || yPadding1 != xPadding1)
            {
                m_indexBuffer.push_back(static_cast<uint>(x + gridCount * rowStride + 2));
                m_indexBuffer.push_back(static_cast<uint>(x + (gridCount - 1) * rowStride + 1));
                m_indexBuffer.push_back(static_cast<uint>(x + (gridCount - 1) * rowStride + 2));
                count += 3;
            }
        }
    }

    if (xPadding0)
    {
        for (int y = 0; y < static_cast<int>(gridCount); y += 2)
        {
            m_indexBuffer.push_back(static_cast<uint>(y * rowStride));
            m_indexBuffer.push_back(static_cast<uint>((y + 1) * rowStride + 1));
            m_indexBuffer.push_back(static_cast<uint>((y + 2) * rowStride));
            count += 3;

            if (y != 0 || xPadding0 != yPadding0)
            {
                m_indexBuffer.push_back(static_cast<uint>(y * rowStride));
                m_indexBuffer.push_back(static_cast<uint>(y * rowStride + 1));
                m_indexBuffer.push_back(static_cast<uint>((y + 1) * rowStride + 1));
                count += 3;
            }

            if (y != static_cast<int>(gridCount) - 2 || xPadding0 != yPadding1)
            {
                m_indexBuffer.push_back(static_cast<uint>((y + 2) * rowStride));
                m_indexBuffer.push_back(static_cast<uint>((y + 1) * rowStride + 1));
                m_indexBuffer.push_back(static_cast<uint>((y + 2) * rowStride + 1));
                count += 3;
            }
        }
    }

    return count;
}

void Quadtree::initGeometry()
{
    const uint dimensionPlusOne = m_cellCount + 1u;
    const size_t pointCount = static_cast<size_t>(dimensionPlusOne) * dimensionPlusOne;
    m_gridPoints.resize(pointCount);

    for (uint row = 0; row <= m_cellCount; ++row)
    {
        for (uint col = 0; col <= m_cellCount; ++col)
        {
            const size_t index = static_cast<size_t>(row) * dimensionPlusOne + col;
            m_gridPoints[index].x = static_cast<float>(col);
            m_gridPoints[index].y = static_cast<float>(row);
        }
    }

    uint topologyIndex = 0;
    uint topoType = 0;
    for (uint j = 0; j < 2; ++j)
    {
        for (uint k = 0; k < 2; ++k)
        {
            topoType = j + 2 * k;
            for (uint l = 0; l < 2; ++l)
            {
                for (uint i = 0; i < 2; ++i)
                {
                    NodeTopology& topo = m_nodeTopologies[topologyIndex++];
                    topo.indexOffset = static_cast<uint>(m_indexBuffer.size());
                    topo.indexCount = static_cast<uint>(generateInnerIndices(j, k, l, i));
                    topo.indexCount += static_cast<uint>(generateSkirtIndices(j, k, l, i));
                    topo.topologyId = topoType /* j + 2 * k + 4 * i + 8 * l */;
                    topoType += 4;
                }
            }
        }
    }
}

int64_t Quadtree::buildNodeList(QuadNode* node)
{
    if (!isNodeVisible(*node))
        return -1;

    const float nodeSize = node->size;
    const float invGrid = 1.0f / static_cast<float>(m_cellCount);
    const float scale = ((nodeSize * invGrid) * (m_geomorphingDegree + 1.0f) * (m_viewportHeight * m_projMatrix.m[5])) * 0.5f;
    const float quarterSize = nodeSize * 0.25f;
    const float sampleBaseX = node->x + quarterSize * 0.5f;
    const float sampleBaseY = node->y + quarterSize * 0.5f;
    const float deltaZ = m_meanSeaLevel - m_cameraZ;
    const float deltaZ2 = deltaZ * deltaZ;

    const float sampleY0 = sampleBaseY - m_cameraY;
    const float sampleY1 = sampleY0 + quarterSize;
    const float sampleY2 = sampleY0 + quarterSize * 2.f;
    const float sampleY3 = sampleY0 + quarterSize * 3.f;

    float maxMetric = 0.0f;
    for (uint i = 0; i < 4; ++i)
    {
        const float sampleX = sampleBaseX + static_cast<float>(i) * quarterSize - m_cameraX;
        const float sampleX2 = sampleX * sampleX;

        float metric = scale / std::sqrt(sampleX2 + sampleY0 * sampleY0 + deltaZ2);
        metric = std::max(metric, scale / std::sqrt(sampleX2 + sampleY1 * sampleY1 + deltaZ2));
        metric = std::max(metric, scale / std::sqrt(sampleX2 + sampleY2 * sampleY2 + deltaZ2));
        metric = std::max(metric, scale / std::sqrt(sampleX2 + sampleY3 * sampleY3 + deltaZ2));

        maxMetric = std::max(maxMetric, metric);
    }

    if (maxMetric > m_maxEdgeLength && nodeSize > m_minPatchLength)
    {
        const float childSize = nodeSize * 0.5f;
        QuadNode child0{node->x, node->y, childSize, -1, -1, -1, -1, node->depth + 1, 0.0f};
        QuadNode child1{node->x + childSize, node->y, childSize, -1, -1, -1, -1, node->depth + 1, 0.0f};
        QuadNode child2{node->x + childSize, node->y + childSize, childSize, -1, -1, -1, -1, node->depth + 1, 0.0f};
        QuadNode child3{node->x, node->y + childSize, childSize, -1, -1, -1, -1, node->depth + 1, 0.0f};

        node->child0 = static_cast<int>(buildNodeList(&child0));
        node->child1 = static_cast<int>(buildNodeList(&child1));
        node->child2 = static_cast<int>(buildNodeList(&child2));
        node->child3 = static_cast<int>(buildNodeList(&child3));

        if (node->child0 < 0 && node->child1 < 0 && node->child2 < 0 && node->child3 < 0)
            return -1;
    }

    calculateDistanceToCameraSquared(*node);
    const int64_t index = static_cast<int64_t>(m_nodeList.size());
    m_nodeList.push_back(*node);

    if (static_cast<uint>(node->depth) > m_maxDepth)
        m_maxDepth = static_cast<uint>(node->depth);

    return index;
}

void Quadtree::calculateDistanceToCameraSquared(QuadNode& node) const
{
    const float halfSize = node.size * 0.5f;
    const float dx = node.x + halfSize - m_cameraX;
    const float dy = node.y + halfSize - m_cameraY;
    node.distanceSquared = dx * dx + dy * dy;
}

bool Quadtree::compareQuadNodesForFrontToBackSorting(const QuadNode& a, const QuadNode& b)
{
    return a.distanceSquared < b.distanceSquared;
}

float Quadtree::estimateGridCellEdgeLength(const QuadNode& node) const
{
    const float invGrid = 1.0f / static_cast<float>(m_cellCount);
    const float scale = ((node.size * invGrid) * (m_geomorphingDegree + 1.0f) * (m_viewportHeight * m_projMatrix.m[5])) * 0.5f;
    const float quarterSize = node.size * 0.25f;
    const float sampleBaseX = node.x + quarterSize * 0.5f;
    const float sampleBaseY = node.y + quarterSize * 0.5f;
    const float deltaZ = m_meanSeaLevel - m_cameraZ;
    const float deltaZ2 = deltaZ * deltaZ;

    const float sampleY0 = sampleBaseY - m_cameraY;
    const float sampleY1 = sampleY0 + quarterSize;
    const float sampleY2 = sampleY0 + quarterSize * 2.f;
    const float sampleY3 = sampleY0 + quarterSize * 3.f;

    float maxEdge = 0.0f;
    for (uint i = 0; i < 4; ++i)
    {
        const float sampleX = sampleBaseX + static_cast<float>(i) * quarterSize - m_cameraX;
        const float sampleX2 = sampleX * sampleX;

        float metric = scale / std::sqrt(sampleX2 + sampleY0 * sampleY0 + deltaZ2);
        metric = std::max(metric, scale / std::sqrt(sampleX2 + sampleY1 * sampleY1 + deltaZ2));
        metric = std::max(metric, scale / std::sqrt(sampleX2 + sampleY2 * sampleY2 + deltaZ2));
        metric = std::max(metric, scale / std::sqrt(sampleX2 + sampleY3 * sampleY3 + deltaZ2));

        maxEdge = std::max(maxEdge, metric);
    }

    return maxEdge;
}

int64_t Quadtree::findIndexOfLeafContainingPoint(const float2& point) const
{
    for (const QuadNode& root : m_rootNodes)
    {
        if (!pointInsideNode(root, point))
            continue;

        if (root.isLeaf())
            return -1;

        int children[4] = { root.child0, root.child1, root.child2, root.child3 };

        while (true)
        {
            int nextIndex = -1;
            for (int i = 0; i < 4; ++i)
            {
                int idx = children[i];
                if (idx < 0)
                    continue;

                const QuadNode& candidate = m_nodeList[idx];
                if (pointInsideNode(candidate, point))
                {
                    if (candidate.isLeaf())
                        return idx;

                    children[0] = candidate.child0;
                    children[1] = candidate.child1;
                    children[2] = candidate.child2;
                    children[3] = candidate.child3;
                    nextIndex = idx;
                    break;
                }
            }

            if (nextIndex < 0)
                return -1;
        }
    }

    return -1;
}

int64_t Quadtree::findIndexOfNodeOfThisDepthContainingPoint(const float2& point, uint depth) const
{
    if (depth >= m_nodesByDepth.size())
        return -1;

    for (uint nodeIndex : m_nodesByDepth[depth])
    {
        const QuadNode& node = m_nodeList[nodeIndex];
        if (pointInsideNode(node, point))
            return nodeIndex;
    }

    return -1;
}

int64_t Quadtree::findBottomNeighborWithSameDepth(const QuadNode& node) const
{
    if (node.depth < 0 || static_cast<size_t>(node.depth) >= m_nodesByDepth.size())
        return -1;

    const auto& depthNodes = m_nodesByDepth[static_cast<size_t>(node.depth)];
    if (depthNodes.empty())
        return -1;

    const float queryX = node.x + node.size * 0.5f;
    const float queryY = node.y - node.size * 0.5f;

    for (uint idx : depthNodes)
    {
        const QuadNode& candidate = m_nodeList[idx];
        if (queryX >= candidate.x && queryX <= candidate.x + candidate.size &&
            queryY >= candidate.y && queryY <= candidate.y + candidate.size)
        {
            return idx;
        }
    }

    return -1;
}

int64_t Quadtree::findLeftNeighborWithSameDepth(const QuadNode& node) const
{
    if (node.depth < 0 || static_cast<size_t>(node.depth) >= m_nodesByDepth.size())
        return -1;

    const auto& depthNodes = m_nodesByDepth[static_cast<size_t>(node.depth)];
    if (depthNodes.empty())
        return -1;

    const float queryX = node.x - node.size * 0.5f;
    const float queryY = node.y + node.size * 0.5f;

    for (uint idx : depthNodes)
    {
        const QuadNode& candidate = m_nodeList[idx];
        if (queryX >= candidate.x && queryX <= candidate.x + candidate.size &&
            queryY >= candidate.y && queryY <= candidate.y + candidate.size)
        {
            return idx;
        }
    }

    return -1;
}

int64_t Quadtree::findRightNeighborWithSameDepth(const QuadNode& node) const
{
    if (node.depth < 0 || static_cast<size_t>(node.depth) >= m_nodesByDepth.size())
        return -1;

    const auto& depthNodes = m_nodesByDepth[static_cast<size_t>(node.depth)];
    if (depthNodes.empty())
        return -1;

    const float queryX = node.x + node.size * 1.5f;
    const float queryY = node.y + node.size * 0.5f;

    for (uint idx : depthNodes)
    {
        const QuadNode& candidate = m_nodeList[idx];
        if (queryX >= candidate.x && queryX <= candidate.x + candidate.size &&
            queryY >= candidate.y && queryY <= candidate.y + candidate.size)
        {
            return idx;
        }
    }

    return -1;
}

int64_t Quadtree::findTopNeighborWithSameDepth(const QuadNode& node) const
{
    if (node.depth < 0 || static_cast<size_t>(node.depth) >= m_nodesByDepth.size())
        return -1;

    const auto& depthNodes = m_nodesByDepth[static_cast<size_t>(node.depth)];
    if (depthNodes.empty())
        return -1;

    const float queryX = node.x + node.size * 0.5f;
    const float queryY = node.y + node.size * 1.5f;

    for (uint idx : depthNodes)
    {
        const QuadNode& candidate = m_nodeList[idx];
        if (queryX >= candidate.x && queryX <= candidate.x + candidate.size &&
            queryY >= candidate.y && queryY <= candidate.y + candidate.size)
        {
            return idx;
        }
    }

    return -1;
}

void Quadtree::rebuildSearchAccelerator()
{
    m_nodesByDepth.clear();
    m_nodesByDepth.resize(m_maxDepth + 1u); // note that m_maxDepth must be updated

    for (uint nodeIndex = 0; nodeIndex < m_nodeList.size(); ++nodeIndex)
    {
        const QuadNode& node = m_nodeList[nodeIndex];
        if (!node.isLeaf())
        {
            if (node.depth >= 0 && static_cast<size_t>(node.depth) < m_nodesByDepth.size())
                m_nodesByDepth[static_cast<size_t>(node.depth)].push_back(nodeIndex);
        }
    }
}

bool Quadtree::refineNodeList()
{
    rebuildSearchAccelerator();

    bool anyRefined = false;

    // pseudo: the node count is re-read every iteration, so children appended in this pass are visited too
    for (size_t nodeIndex = 0; nodeIndex < m_nodeList.size(); ++nodeIndex)
    {
        QuadNode& node = m_nodeList[nodeIndex];
        if (!node.isLeaf())
            continue;

        if (static_cast<uint>(node.depth) >= m_maxDepth - 1)
            continue;

        bool refineLeft = false;
        bool refineRight = false;
        bool refineBottom = false;
        bool refineTop = false;

        const int64_t leftNeighbor = findLeftNeighborWithSameDepth(node);
        if (leftNeighbor >= 0) {
            auto& childNode = m_nodeList[leftNeighbor];
            // 1, 2
            if ((childNode.child1 >= 0 && !m_nodeList[childNode.child1].isLeaf()) ||
                (childNode.child2 >= 0 && !m_nodeList[childNode.child2].isLeaf())) {
                refineLeft = true;
            }
        }

        const int64_t rightNeighbor = findRightNeighborWithSameDepth(node);
        if (rightNeighbor >= 0) {
            auto& childNode = m_nodeList[rightNeighbor];
            // 0, 3
            if ((childNode.child0 >= 0 && !m_nodeList[childNode.child0].isLeaf()) ||
                (childNode.child3 >= 0 && !m_nodeList[childNode.child3].isLeaf())) {
                refineRight = true;
            }
        }

        const int64_t topNeighbor = findTopNeighborWithSameDepth(node);
        if (topNeighbor >= 0) {
            auto& childNode = m_nodeList[topNeighbor];
            // 0, 1
            if ((childNode.child0 >= 0 && !m_nodeList[childNode.child0].isLeaf()) ||
                (childNode.child1 >= 0 && !m_nodeList[childNode.child1].isLeaf())) {
                refineTop = true;
            }
        }

        const int64_t bottomNeighbor = findBottomNeighborWithSameDepth(node); // 0, 1
        if (bottomNeighbor >= 0) {
            auto& childNode = m_nodeList[bottomNeighbor];
            // 2, 3
            if ((childNode.child2 >= 0 && !m_nodeList[childNode.child2].isLeaf()) ||
                (childNode.child3 >= 0 && !m_nodeList[childNode.child3].isLeaf())) {
                refineBottom = true;
            }
        }

        if (refineLeft || refineRight || refineBottom || refineTop)
        {
            const float childSize = node.size * 0.5f;
            const int childDepth = node.depth + 1;
            QuadNode child0{node.x, node.y, childSize, -1, -1, -1, -1, childDepth, 0.0f};
            QuadNode child1{node.x + childSize, node.y, childSize, -1, -1, -1, -1, childDepth, 0.0f};
            QuadNode child2{node.x + childSize, node.y + childSize, childSize, -1, -1, -1, -1, childDepth, 0.0f};
            QuadNode child3{node.x, node.y + childSize, childSize, -1, -1, -1, -1, childDepth, 0.0f};

            calculateDistanceToCameraSquared(child0);
            calculateDistanceToCameraSquared(child1);
            calculateDistanceToCameraSquared(child2);
            calculateDistanceToCameraSquared(child3);

            if (m_nodesByDepth.size() < static_cast<size_t>(childDepth) + 1u)
                m_nodesByDepth.resize(childDepth + 1u);

            // pseudo: each push_back may reallocate m_nodeList, so the parent is re-derived via m_nodeList[nodeIndex]
            m_nodeList[nodeIndex].child0 = static_cast<int>(m_nodeList.size());
            m_nodeList.push_back(child0);
            m_nodesByDepth[childDepth].push_back(m_nodeList[nodeIndex].child0);
            m_nodeList[nodeIndex].child1 = static_cast<int>(m_nodeList.size());
            m_nodeList.push_back(child1);
            m_nodesByDepth[childDepth].push_back(m_nodeList[nodeIndex].child1);
            m_nodeList[nodeIndex].child2 = static_cast<int>(m_nodeList.size());
            m_nodeList.push_back(child2);
            m_nodesByDepth[childDepth].push_back(m_nodeList[nodeIndex].child2);
            m_nodeList[nodeIndex].child3 = static_cast<int>(m_nodeList.size());
            m_nodeList.push_back(child3);
            m_nodesByDepth[childDepth].push_back(m_nodeList[nodeIndex].child3);

            anyRefined = true;
        }
    }

    return anyRefined;
}

const Quadtree::NodeTopology* Quadtree::selectNodeTopology(const QuadNode& node) const
{
    const float halfMin = 0.5f * m_minPatchLength;
    const float halfSize = node.size * 0.5f;

    float2 sample0{ node.x - halfMin, node.y + halfSize };
    float2 sample1{ node.x + halfMin + node.size, node.y + halfSize };
    float2 sample2{ node.x + halfSize, node.y - halfMin };
    float2 sample3{ node.x + halfSize, node.y + halfMin + node.size };

    const int64_t leaf0 = findIndexOfLeafContainingPoint(sample0);
    const int64_t leaf1 = findIndexOfLeafContainingPoint(sample1);
    const int64_t leaf2 = findIndexOfLeafContainingPoint(sample2);
    const int64_t leaf3 = findIndexOfLeafContainingPoint(sample3);

    const bool flag0 = leaf0 >= 0 && (m_nodeList[leaf0].size / node.size) > 1.99f;
    const bool flag1 = leaf1 >= 0 && (m_nodeList[leaf1].size / node.size) > 1.99f;
    const bool flag2 = leaf2 >= 0 && (m_nodeList[leaf2].size / node.size) > 1.99f;
    const bool flag3 = leaf3 >= 0 && (m_nodeList[leaf3].size / node.size) > 1.99f;

    const uint offset = 8 * static_cast<uint>(flag0) + 4 * static_cast<uint>(flag1) +
                        2 * static_cast<uint>(flag2) + +static_cast<uint>(flag3);

    return &m_nodeTopologies[offset];
}

bool Quadtree::isNodeVisible(const QuadNode& node) const
{
    const float x0 = node.x - m_timeValue;
    const float y0 = node.y - m_timeValue;
    const float x1 = (node.size + (m_timeValue + m_timeValue)) + x0; // node.x + node.size + eps
    const float y1 = (node.size + (m_timeValue + m_timeValue)) + y0; // node.y + node.size + eps
    const float z0 = m_meanSeaLevel - m_timeValue;
    const float z1 = m_meanSeaLevel + m_timeValue;

    const std::array<float4, 8> nodeCorners = {
        float4{x0, y0, z0, 1.f},
        float4{x1, y0, z0, 1.f},
        float4{x1, y1, z0, 1.f},
        float4{x0, y1, z0, 1.f},
        float4{x0, y0, z1, 1.f},
        float4{x1, y0, z1, 1.f},
        float4{x1, y1, z1, 1.f},
        float4{x0, y1, z1, 1.f}
    };

    bool allLeft = true;
    bool allRight = true;
    bool allBottom = true;
    bool allTop = true;
    bool allNear = true;
    bool allFar = true;

    for (const auto& pt : nodeCorners)
    {
        float4 clip = mul(pt, m_viewProjMatrix);

        allLeft &= clip.x < -clip.w;
        allRight &= clip.x > clip.w;
        allBottom &= clip.y < -clip.w;
        allTop &= clip.y > clip.w;
        allNear &= clip.z < 0.0f;
        allFar &= clip.z > clip.w;
    }

    if (allLeft || allRight || allBottom || allTop || allNear || allFar)
        return false;

    return true;
}

bool Quadtree::getNodes(
    const float4x4& viewMatrix,
    const float4x4& projMatrix,
    const float2& viewportSize,
    float timeValue,
    uint* nodeCount,
    GFSDK_WaveWorks_Quadtree_NodeRenderingProperties** nodeProperties)
{
    const auto start = std::chrono::steady_clock::now();

    m_viewportWidth = viewportSize.x;
    m_viewportHeight = viewportSize.y;
    m_timeValue = timeValue;

    m_viewMatrix = viewMatrix;
    m_projMatrix = projMatrix;
    m_viewProjMatrix = mul(viewMatrix, projMatrix);

    float4 cameraPos = affine_inverse(viewMatrix).r3;
    m_cameraX = cameraPos.x;
    m_cameraY = cameraPos.y;
    m_cameraZ = cameraPos.z;

    m_one = 1.0f;

    m_scaleFactor = ((m_geomorphingDegree * 0.5f) * m_maxEdgeLength)
        / (m_viewportHeight * projMatrix.m[5]);


    m_rootNodes.clear();
    m_nodeList.clear();
    m_sortedNodeList.clear();
    m_renderProps.clear();
    m_maxDepth = 0;

    const float tileSize = static_cast<float>(1u << m_maxLODNumber) * m_minPatchLength;
    const float step = tileSize / static_cast<float>(m_cellCount >> 1);
    const float rawX = std::floor((m_cameraX / step) + 0.5f);
    const float rawY = std::floor((m_cameraY / step) + 0.5f);
    const float baseX = rawX * step;
    const float baseY = rawY * step;

    QuadNode roots[4] = {
        { baseX, baseY, tileSize, -1, -1, -1, -1, 0, 0.0f },
        { baseX, baseY - tileSize, tileSize, -1, -1, -1, -1, 0, 0.0f },
        { baseX - tileSize, baseY, tileSize, -1, -1, -1, -1, 0, 0.0f },
        { baseX - tileSize, baseY - tileSize, tileSize, -1, -1, -1, -1, 0, 0.0f }
    };

    for (int i = 0; i < 4; ++i)
    {
        calculateDistanceToCameraSquared(roots[i]);
        if (buildNodeList(&roots[i]) >= 0)
            m_rootNodes.push_back(roots[i]);
    }

    const std::size_t initialNodeCount = m_nodeList.size();
    uint numRefinementIters = 0;
    for (; refineNodeList() != 0; ++numRefinementIters)
        ;

    const std::size_t finalNodeCount = m_nodeList.size();
    const std::size_t createdNodes = finalNodeCount - initialNodeCount;

    m_sortedNodeList = m_nodeList;
    std::sort(m_sortedNodeList.begin(), m_sortedNodeList.end(), compareQuadNodesForFrontToBackSorting);

    m_renderProps.clear();

    const float halfMinPathLength = m_minPatchLength * 0.5f;

    for (const QuadNode& node : m_sortedNodeList)
    {
        if (!node.isLeaf())
            continue;

        const float halfSize = node.size * 0.5f;
        const float2 topLeft{ node.x - halfMinPathLength, node.y + halfSize };
        const float2 topRight{ node.x + halfMinPathLength + node.size, node.y + halfSize };
        const float2 bottomLeft{ node.x + halfSize, node.y - halfMinPathLength };
        const float2 bottomRight{ node.x + halfSize, node.y + halfMinPathLength + node.size };

        const int64_t leaf0 = findIndexOfLeafContainingPoint(topLeft);
        const int64_t leaf1 = findIndexOfLeafContainingPoint(topRight);
        const int64_t leaf2 = findIndexOfLeafContainingPoint(bottomLeft);
        const int64_t leaf3 = findIndexOfLeafContainingPoint(bottomRight);

        const bool flag0 = leaf0 >= 0 && (m_nodeList[leaf0].size / node.size) > 1.99f;
        const bool flag1 = leaf1 >= 0 && (m_nodeList[leaf1].size / node.size) > 1.99f;
        const bool flag2 = leaf2 >= 0 && (m_nodeList[leaf2].size / node.size) > 1.99f;
        const bool flag3 = leaf3 >= 0 && (m_nodeList[leaf3].size / node.size) > 1.99f;

        uint topologyIndex = 8u * static_cast<uint>(flag0)
            + 4u * static_cast<uint>(flag1)
            + 2u * static_cast<uint>(flag2)
            + static_cast<uint>(flag3);

        const NodeTopology& topology = m_nodeTopologies[topologyIndex];

        float sign = 1.0f;
        if (m_generateDiamondPattern && ((node.depth & 1) != 0))
            sign = -1.0f;

        GFSDK_WaveWorks_Quadtree_NodeRenderingProperties props{};
        props.start_index = topology.indexOffset;
        props.num_indices = topology.indexCount;
        props.patch_type = topology.topologyId;
        props.patch_worldspace_origin.x = node.x;
        props.patch_worldspace_origin.y = node.y;
        props.patch_worldspace_scale = node.size / static_cast<float>(m_cellCount);
        props.geomorphing_distance_constant = (static_cast<float>(m_cellCount) * m_scaleFactor) / node.size;
        props.geomorphing_sign = sign;

        m_renderProps.push_back(props);
    }

    const auto end = std::chrono::steady_clock::now();
    const float elapsedMs = std::chrono::duration<float, std::milli>(end - start).count();

    m_stats.num_quadtree_nodes_created = static_cast<uint>(m_nodeList.size());
    m_stats.num_quadtree_nodes_for_rendering = static_cast<uint>(m_renderProps.size());
    m_stats.num_refinement_nodes_added = static_cast<uint>(createdNodes);
    m_stats.num_refinement_iterations = numRefinementIters;
    m_stats.CPU_quadtree_update_time = elapsedMs;

    if (!m_renderProps.empty())
    {
        *nodeCount = static_cast<uint>(m_renderProps.size());
        *nodeProperties = m_renderProps.data();
        return true;
    }

    *nodeCount = 0;
    *nodeProperties = nullptr;
    return false;
}

void Quadtree::getStats(GFSDK_WaveWorks_Quadtree_Stats& stats) const
{
    stats = m_stats;
}

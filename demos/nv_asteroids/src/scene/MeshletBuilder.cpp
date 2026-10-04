#include "scene/MeshletBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace donut::math;

// ------------------------------------------------------------------------------------------------
// Forsyth vertex cache optimiser

namespace
{
    constexpr uint32_t c_MaxCacheSize = 64;
    constexpr uint32_t c_MaxValence = 254;
    constexpr uint32_t c_ValenceTableSize = 32;
    constexpr float c_ScoreScale = 7281.f;

    struct ScoreTables
    {
        int cache[c_MaxCacheSize];
        int valence[c_ValenceTableSize];

        ScoreTables()
        {
            for (uint32_t p = 0; p < c_MaxCacheSize; ++p)
            {
                float score;
                if (p < 3)
                    score = 0.75f;
                else
                    score = powf(1.f - float(p - 3) / float(c_MaxCacheSize - 3), 1.5f);
                cache[p] = int(score * c_ScoreScale);
            }
            valence[0] = 0;
            for (uint32_t v = 1; v < c_ValenceTableSize; ++v)
                valence[v] = int(2.f * powf(float(v), -0.5f) * c_ScoreScale);
        }
    };

    const ScoreTables& GetScoreTables()
    {
        static const ScoreTables tables;
        return tables;
    }

    int VertexScore(int cachePosition, uint32_t valence, uint32_t cacheSize)
    {
        if (valence == 0)
            return 0;

        const ScoreTables& tables = GetScoreTables();
        int score = 0;
        if (cachePosition >= 0 && uint32_t(cachePosition) < cacheSize)
            score += tables.cache[cachePosition];
        if (valence < c_ValenceTableSize)
            score += tables.valence[valence];
        return score;
    }
}

void OptimizeVertexCache(uint32_t* indices, size_t numIndices, uint32_t cacheSize)
{
    const size_t numTris = numIndices / 3;
    if (numTris == 0)
        return;

    cacheSize = std::min(cacheSize, c_MaxCacheSize);

    uint32_t numVertices = 0;
    for (size_t i = 0; i < numTris * 3; ++i)
        numVertices = std::max(numVertices, indices[i] + 1);

    std::vector<uint32_t> valence(numVertices, 0);
    for (size_t i = 0; i < numTris * 3; ++i)
    {
        if (++valence[indices[i]] > c_MaxValence)
            return;
    }

    // Vertex -> triangle adjacency
    std::vector<uint32_t> adjacencyStart(numVertices + 1, 0);
    for (uint32_t v = 0; v < numVertices; ++v)
        adjacencyStart[v + 1] = adjacencyStart[v] + valence[v];
    std::vector<uint32_t> adjacency(numTris * 3);
    {
        std::vector<uint32_t> fill(adjacencyStart.begin(), adjacencyStart.end() - 1);
        for (size_t t = 0; t < numTris; ++t)
            for (int k = 0; k < 3; ++k)
                adjacency[fill[indices[t * 3 + k]]++] = uint32_t(t);
    }

    std::vector<int> vertexScore(numVertices);
    std::vector<int> cachePosition(numVertices, -1);
    for (uint32_t v = 0; v < numVertices; ++v)
        vertexScore[v] = VertexScore(-1, valence[v], cacheSize);

    std::vector<int> triScore(numTris);
    std::vector<bool> emitted(numTris, false);
    int bestTri = -1;
    int bestScore = -1;
    for (size_t t = 0; t < numTris; ++t)
    {
        triScore[t] = vertexScore[indices[t * 3]] + vertexScore[indices[t * 3 + 1]] + vertexScore[indices[t * 3 + 2]];
        if (triScore[t] > bestScore)
        {
            bestScore = triScore[t];
            bestTri = int(t);
        }
    }

    std::vector<int> cache(cacheSize + 3, -1);
    std::vector<uint32_t> outTris;
    outTris.reserve(numTris);
    size_t cursor = 0;

    while (bestTri >= 0)
    {
        emitted[bestTri] = true;
        outTris.push_back(uint32_t(bestTri));

        for (int k = 0; k < 3; ++k)
        {
            const uint32_t v = indices[bestTri * 3 + k];

            // Move the vertex to cache slot k, shifting the entries in between down by one.
            int p = cachePosition[v] >= 0 ? cachePosition[v] : int(cacheSize) + k;
            for (int i = p - 1; i >= k; --i)
            {
                cache[i + 1] = cache[i];
                if (cache[i + 1] >= 0)
                    cachePosition[cache[i + 1]] = i + 1;
            }
            cache[k] = int(v);
            cachePosition[v] = k;

            // Remove the triangle from the vertex's adjacency list.
            uint32_t* begin = &adjacency[adjacencyStart[v]];
            uint32_t* end = begin + valence[v];
            for (uint32_t* it = begin; it != end; ++it)
            {
                if (*it == uint32_t(bestTri))
                {
                    *it = *(end - 1);
                    break;
                }
            }
            --valence[v];
        }

        // Evict the overflow slots and propagate the score changes.
        for (uint32_t i = 0; i < cacheSize + 3; ++i)
        {
            const int v = cache[i];
            if (v < 0)
                break;

            if (i >= cacheSize)
            {
                cache[i] = -1;
                cachePosition[v] = -1;
            }

            const int newScore = VertexScore(cachePosition[v], valence[v], cacheSize);
            const int delta = newScore - vertexScore[v];
            vertexScore[v] = newScore;
            if (delta != 0)
            {
                for (uint32_t a = 0; a < valence[v]; ++a)
                    triScore[adjacency[adjacencyStart[v] + a]] += delta;
            }
        }

        // Next triangle: best one adjacent to the cached vertices.
        bestTri = -1;
        bestScore = -1;
        for (uint32_t i = 0; i < cacheSize; ++i)
        {
            const int v = cache[i];
            if (v < 0)
                break;
            for (uint32_t a = 0; a < valence[v]; ++a)
            {
                const uint32_t t = adjacency[adjacencyStart[v] + a];
                if (!emitted[t] && triScore[t] > bestScore)
                {
                    bestScore = triScore[t];
                    bestTri = int(t);
                }
            }
        }

        if (bestTri < 0)
        {
            while (cursor < numTris && emitted[cursor])
                ++cursor;
            if (cursor < numTris)
                bestTri = int(cursor);
        }
    }

    std::vector<uint32_t> reordered(numTris * 3);
    for (size_t i = 0; i < outTris.size(); ++i)
        memcpy(&reordered[i * 3], &indices[outTris[i] * 3], 3 * sizeof(uint32_t));
    memcpy(indices, reordered.data(), reordered.size() * sizeof(uint32_t));
}

// ------------------------------------------------------------------------------------------------
// Greedy meshlet clustering

namespace
{
    class MeshletAccumulator
    {
    public:
        MeshletAccumulator(SceneMeshletData& data, uint32_t maxVerts, uint32_t maxPrims)
            : m_Data(data), m_MaxVerts(maxVerts), m_MaxPrims(maxPrims)
        {
            m_Verts.reserve(maxVerts);
            m_Prims.reserve(maxPrims * 3);
        }

        bool Empty() const { return m_Verts.empty(); }

        // Returns true if the current meshlet had to be flushed to fit the triangle.
        bool AddTriangle(const uint32_t tri[3])
        {
            bool flushed = false;
            if (m_Prims.size() + 3 > size_t(m_MaxPrims) * 3)
            {
                Flush();
                flushed = true;
            }
            else
            {
                uint32_t newVerts = 0;
                for (int k = 0; k < 3; ++k)
                    if (std::find(m_Verts.begin(), m_Verts.end(), tri[k]) == m_Verts.end())
                        ++newVerts;
                if (m_Verts.size() + newVerts > m_MaxVerts)
                {
                    Flush();
                    flushed = true;
                }
            }

            for (int k = 0; k < 3; ++k)
            {
                auto it = std::find(m_Verts.begin(), m_Verts.end(), tri[k]);
                size_t local;
                if (it != m_Verts.end())
                    local = size_t(it - m_Verts.begin());
                else
                {
                    m_Verts.push_back(tri[k]);
                    local = m_Verts.size() - 1;
                }
                m_Prims.push_back(uint8_t(local));
            }
            return flushed;
        }

        void Flush()
        {
            chunk2018::MeshletHeader header;
            header.packedVertexCount = uint32_t(uint8_t(m_Verts.size())) << 24;
            header.packedPrimCount = uint32_t(uint8_t(m_Prims.size() / 3)) << 24;
            header.vertexOffset = uint32_t(m_Data.vertexIndices.size());
            header.primOffset = uint32_t(m_Data.primIndices.size());
            m_Data.meshlets.push_back(header);

            m_Data.vertexIndices.insert(m_Data.vertexIndices.end(), m_Verts.begin(), m_Verts.end());
            m_Data.primIndices.insert(m_Data.primIndices.end(), m_Prims.begin(), m_Prims.end());
            m_Verts.clear();
            m_Prims.clear();
        }

    private:
        SceneMeshletData& m_Data;
        uint32_t m_MaxVerts;
        uint32_t m_MaxPrims;
        std::vector<uint32_t> m_Verts;
        std::vector<uint8_t> m_Prims;
    };

    void BuildClusters(SceneMeshletData& data, const std::vector<uint32_t>& indices,
        const std::vector<MeshletSubset>& subsets, const MeshletBuildOptions& options)
    {
        MeshletAccumulator acc(data, options.maxVerts, options.maxPrims);
        uint2 current(0, 1);    // {firstMeshlet, numMeshlets} of the subset being built
        bool first = true;

        for (const MeshletSubset& subset : subsets)
        {
            if (subset.numPrims == 0)
                continue;

            if (!first)
            {
                acc.Flush();
                data.subsets.push_back(current);
                current.x += current.y;
                current.y = 1;
            }
            first = false;

            const uint32_t end = subset.indexOffset + subset.numPrims * 3;
            for (uint32_t p = subset.indexOffset; p + 3 <= end && size_t(p) + 3 <= indices.size(); p += 3)
            {
                const uint32_t tri[3] = { indices[p], indices[p + 1], indices[p + 2] };
                if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2])
                    continue;
                if (acc.AddTriangle(tri))
                    ++current.y;
            }
        }

        if (!acc.Empty())
            acc.Flush();
        if (!first && current.y > 0)
            data.subsets.push_back(current);
    }

    float3 ReadPosition(const MeshletBuildDesc& desc, uint32_t index)
    {
        float3 p;
        memcpy(&p, desc.positions + size_t(index) * desc.positionStride, sizeof(float3));
        return p;
    }

    void ComputeCullingData(SceneMeshletData& data, const MeshletBuildDesc& desc)
    {
        // deviation: the 2018 code starts the mesh bounds at vertex 1 (vertex 0 is skipped); include it.
        box3 meshBounds = box3::empty();
        for (uint32_t v = 0; v < desc.numVertices; ++v)
            meshBounds |= ReadPosition(desc, v);

        const float3 extent = meshBounds.diagonal();
        for (chunk2018::MeshletHeader& m : data.meshlets)
        {
            box3 b = box3::empty();
            const uint32_t numPrims = m.primCount();
            for (uint32_t i = 0; i < numPrims * 3; ++i)
            {
                const uint32_t vertex = data.vertexIndices[m.vertexOffset + data.primIndices[m.primOffset + i]];
                b |= ReadPosition(desc, vertex);
            }

            uint32_t qmin[3], qmax[3];
            for (int axis = 0; axis < 3; ++axis)
            {
                // deviation: guard flat axes (the 2018 code divides by zero there).
                const float range = extent[axis] > 0.f ? extent[axis] : 1.f;
                const float nmin = (b.m_mins[axis] - meshBounds.m_mins[axis]) / range;
                const float nmax = (b.m_maxs[axis] - meshBounds.m_mins[axis]) / range;
                qmin[axis] = uint32_t(std::clamp(int(truncf(nmin * 255.f)), 0, 254));
                qmax[axis] = uint32_t(std::clamp(int(ceilf(nmax * 255.f)), 0, 255));
            }
            m.packedVertexCount |= qmin[0] | (qmin[1] << 8) | (qmin[2] << 16);
            m.packedPrimCount |= qmax[0] | (qmax[1] << 8) | (qmax[2] << 16);
        }

        data.bounds = meshBounds;
    }
}

std::unique_ptr<SceneMeshletData> BuildMeshlets(const MeshletBuildDesc& desc, const MeshletBuildOptions& options)
{
    // The 2018 builder only notices the missing position attribute after clustering; check up front.
    if (!desc.positions)
        return nullptr;

    std::vector<uint32_t> indices(desc.indices, desc.indices + desc.numIndices);

    std::vector<MeshletSubset> subsets;
    if (desc.numSubsets)
        subsets.assign(desc.subsets, desc.subsets + desc.numSubsets);
    else
        subsets.push_back({ 0, desc.numIndices / 3, 0 });

    // Make the indices global: each index position is rebased at most once, even when several
    // subsets (one per instance in the demo) reference the same index range.
    if (desc.rebaseSubsets && desc.numSubsets)
    {
        std::vector<bool> rebased(indices.size(), false);
        for (const MeshletSubset& subset : subsets)
        {
            const size_t end = std::min(size_t(subset.indexOffset) + size_t(subset.numPrims) * 3, indices.size());
            for (size_t p = subset.indexOffset; p < end; ++p)
            {
                if (!rebased[p])
                {
                    indices[p] += subset.vertexOffset;
                    rebased[p] = true;
                }
            }
        }
    }

    if (options.optimizeVertexCache)
    {
        for (const MeshletSubset& subset : subsets)
        {
            if (subset.numPrims == 0)
                continue;
            const size_t end = size_t(subset.indexOffset) + size_t(subset.numPrims) * 3;
            if (end <= indices.size())
                OptimizeVertexCache(indices.data() + subset.indexOffset, size_t(subset.numPrims) * 3, options.cacheSize);
        }
    }

    auto data = std::make_unique<SceneMeshletData>();
    BuildClusters(*data, indices, subsets, options);
    ComputeCullingData(*data, desc);
    data->numVertices = desc.numVertices;
    data->numPrims = desc.numIndices / 3;
    return data;
}

// Glue between the shared benchmark contract (common/benchmark.h)
// and the native renderers of this sample. Not part of the upstream sample.

#pragma once

#include "benchmark.h"

#include <dxgi1_5.h>

namespace BenchSupport
{

// Options of the native renderers that are not part of the shared contract (parsed in WinWrapper.cpp).
struct Options
{
    // Native D3D12, static vertex/index data: false = the stock sample's UPLOAD heap, true = a DEFAULT heap
    // buffer filled once (-native_geometry_heap upload|default). Benchmark mode default: DEFAULT heap.
    bool nativeGeometryDefaultHeap = false;
    // Native D3D11 / D3D12: create the flip-model swap chain with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING and present
    // with DXGI_PRESENT_ALLOW_TEARING (-native_present tearing|default). Without it a windowed flip-model
    // swap chain can be paced by the compositor even with sync interval 0. Donut does this too.
    // Benchmark mode default: on when DXGI_FEATURE_PRESENT_ALLOW_TEARING is supported.
    bool nativeAllowTearing = false;
};
inline Options options;

// DXGI_FEATURE_PRESENT_ALLOW_TEARING (the check Donut makes before it uses the flag).
inline bool TearingSupported()
{
    IDXGIFactory5* factory   = nullptr;
    BOOL           supported = FALSE;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&factory))))
    {
        if (FAILED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported, sizeof(supported))))
            supported = FALSE;
        factory->Release();
    }
    return supported != FALSE;
}

// Attributes consecutive main-thread wall-clock segments to benchmark columns with one QPC read
// per boundary:  Lap lap; ...; lap.To(times.update); ...; lap.To(times.render);
class Lap
{
    int64_t m_Last;

public:
    Lap() :
        m_Last(Benchmark::Now()) {}

    void To(double& column)
    {
        const int64_t now = Benchmark::Now();
        column += Benchmark::Ms(now - m_Last);
        m_Last = now;
    }

    // Drops the time since the last boundary (work that belongs to no column, e.g. frame capture).
    void Skip() { m_Last = Benchmark::Now(); }
};

// Per-subset draw statistics, written by the thread that records the subset and summed by the
// main thread after the workers were joined. Padded to avoid false sharing.
struct alignas(64) SubsetStats
{
    uint64_t draws   = 0;
    uint64_t indices = 0;
};

// draw_count  = asteroid draw calls + 1 skybox draw
// index_count = sum of the index counts of the asteroid draws (the skybox is not indexed)
template <class Container>
inline void ReportFrameStats(const Container& stats, size_t count)
{
    uint64_t draws = 1, indices = 0;
    for (size_t i = 0; i < count; ++i)
    {
        draws += stats[i].draws;
        indices += stats[i].indices;
    }
    Benchmark::recorder.SetFrameStats(draws, indices);
}

} // namespace BenchSupport

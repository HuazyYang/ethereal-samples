#pragma once
// D3D11-only helpers for the benchmark contract. Include after <d3d11.h>.
#include "benchmark.h"

#include <d3d11.h>

namespace Benchmark
{

// LUID (decimal form of -adapter_luid) of the adapter a D3D11 device was created on; 0 on failure.
inline uint64_t AdapterLuid(ID3D11Device* device)
{
    IDXGIDevice*  dxgi    = nullptr;
    IDXGIAdapter* adapter = nullptr;
    uint64_t      luid    = 0;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))))
    {
        if (SUCCEEDED(dxgi->GetAdapter(&adapter)))
        {
            DXGI_ADAPTER_DESC d{};
            if (SUCCEEDED(adapter->GetDesc(&d))) luid = LuidToU64(d.AdapterLuid);
            adapter->Release();
        }
        dxgi->Release();
    }
    return luid;
}

// GPU frame time from timestamp queries on the immediate context. Never blocks: results are polled
// with DONOTFLUSH and reported to Benchmark::recorder for the frame they belong to.
// Usage (immediate context, only when Benchmark::config.gpuTiming):
//   timer.Poll(context);           once per frame, OUTSIDE the render/submit columns
//   timer.Begin(device, context);  ... all draws of the frame ...  timer.End(context);  Present
class GpuTimer11
{
    enum { kSlots = 16 };
    struct Slot
    {
        ID3D11Query* begin = nullptr;
        ID3D11Query* end = nullptr;
        ID3D11Query* disjoint = nullptr;
        uint64_t     frame = 0;
        bool         pending = false;
    };
    Slot m_Slots[kSlots];
    int  m_Active = -1;

public:
    GpuTimer11() = default;
    GpuTimer11(const GpuTimer11&) = delete;
    GpuTimer11& operator=(const GpuTimer11&) = delete;
    ~GpuTimer11()
    {
        for (Slot& s : m_Slots)
        {
            if (s.begin) s.begin->Release();
            if (s.end) s.end->Release();
            if (s.disjoint) s.disjoint->Release();
        }
    }

    // Polls the pending queries (GetData with DONOTFLUSH) and reports the finished ones.
    void Poll(ID3D11DeviceContext* context)
    {
        for (Slot& s : m_Slots)
        {
            if (s.pending)
            {
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
                UINT64 t0 = 0, t1 = 0;
                if (context->GetData(s.disjoint, &d, sizeof(d), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                    context->GetData(s.begin, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                    context->GetData(s.end, &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
                {
                    if (!d.Disjoint && d.Frequency) recorder.SetGpuMs(s.frame, double(t1 - t0) * 1000.0 / double(d.Frequency));
                    s.pending = false;
                }
            }
        }
    }

    // Returns false if the queries could not be created (GPU timing is then unavailable, not an error).
    // Takes a free slot; call Poll() first (outside the timed columns) to free the finished ones.
    bool Begin(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        m_Active = -1;
        for (int i = 0; i < kSlots && m_Active < 0; ++i)
            if (!m_Slots[i].pending) m_Active = i;
        if (m_Active < 0) return true; // all slots in flight: skip this frame rather than wait
        Slot& s = m_Slots[m_Active];
        if (!s.begin)
        {
            D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP, 0};
            D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            if (FAILED(device->CreateQuery(&d, &s.begin)) || FAILED(device->CreateQuery(&d, &s.end)) ||
                FAILED(device->CreateQuery(&dj, &s.disjoint)))
            {
                m_Active = -1;
                return false;
            }
        }
        s.frame = recorder.FrameIndex();
        context->Begin(s.disjoint);
        context->End(s.begin);
        return true;
    }

    void End(ID3D11DeviceContext* context)
    {
        if (m_Active < 0) return;
        Slot& s = m_Slots[m_Active];
        context->End(s.end);
        context->End(s.disjoint);
        s.pending = true;
        m_Active  = -1;
    }
};

} // namespace Benchmark

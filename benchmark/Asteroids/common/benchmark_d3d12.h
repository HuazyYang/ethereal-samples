#pragma once
// D3D12-only helpers for the benchmark contract. Include after <d3d12.h>.
#include "benchmark.h"

#include <d3d12.h>

namespace Benchmark
{

// LUID (decimal form of -adapter_luid) of the adapter a D3D12 device was created on.
inline uint64_t AdapterLuid(ID3D12Device* device) { return LuidToU64(device->GetAdapterLuid()); }

// GPU frame time from two timestamp queries per frame, resolved into a readback buffer.
// Usage (only when Benchmark::config.gpuTiming):
//   timer.Init(device, queue) once;
//   timer.Collect()                          once per frame, OUTSIDE the render/submit columns,
//   timer.Begin(firstCommandListOfTheFrame)  before the first GPU work of the frame is recorded,
//   timer.End(lastCommandListOfTheFrame)     after the last GPU work, before that list is closed.
// Both lists must be executed in that order on the queue passed to Init.
// Collect() reads back the slot that the next Begin() reuses (written kSlots frames earlier), without
// any wait. Precondition: the renderer never lets the CPU run kSlots (16) or more frames ahead of the GPU.
// The two EndQuery calls and the ResolveQueryData stay inside the timed columns, which is one reason
// why runs with -gpu_timing are excluded from every headline metric.
class GpuTimer12
{
    enum { kSlots = 16 };
    ID3D12QueryHeap* m_Heap = nullptr;
    ID3D12Resource*  m_Readback = nullptr;
    UINT64           m_Frequency = 0;
    uint64_t         m_Frames[kSlots] = {};
    bool             m_Pending[kSlots] = {};
    int              m_Active = -1;
    uint64_t         m_Counter = 0;

public:
    GpuTimer12() = default;
    GpuTimer12(const GpuTimer12&) = delete;
    GpuTimer12& operator=(const GpuTimer12&) = delete;
    ~GpuTimer12()
    {
        if (m_Readback) m_Readback->Release();
        if (m_Heap) m_Heap->Release();
    }

    bool Init(ID3D12Device* device, ID3D12CommandQueue* queue)
    {
        if (FAILED(queue->GetTimestampFrequency(&m_Frequency)) || m_Frequency == 0) return false;
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 2 * kSlots;
        if (FAILED(device->CreateQueryHeap(&qh, __uuidof(ID3D12QueryHeap), reinterpret_cast<void**>(&m_Heap)))) return false;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = 2 * kSlots * sizeof(UINT64);
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_Readback)));
    }

    bool Ready() const { return m_Readback != nullptr; }

    // Reads back the result of the slot the next Begin() will reuse (Map of the readback buffer).
    void Collect()
    {
        if (!Ready()) return;
        const int slot = int(m_Counter % kSlots);
        if (m_Pending[slot])
        {
            const D3D12_RANGE range{size_t(slot) * 2 * sizeof(UINT64), size_t(slot + 1) * 2 * sizeof(UINT64)};
            void* p = nullptr;
            if (SUCCEEDED(m_Readback->Map(0, &range, &p)))
            {
                const UINT64* t = static_cast<const UINT64*>(p) + size_t(slot) * 2;
                if (t[1] >= t[0]) recorder.SetGpuMs(m_Frames[slot], double(t[1] - t[0]) * 1000.0 / double(m_Frequency));
                const D3D12_RANGE none{0, 0};
                m_Readback->Unmap(0, &none);
            }
            m_Pending[slot] = false;
        }
    }

    void Begin(ID3D12GraphicsCommandList* cmd)
    {
        if (!Ready()) return;
        Collect(); // no-op when the renderer already called it this frame
        const int slot = int(m_Counter++ % kSlots);
        m_Active       = slot;
        m_Frames[slot] = recorder.FrameIndex();
        cmd->EndQuery(m_Heap, D3D12_QUERY_TYPE_TIMESTAMP, UINT(slot) * 2);
    }

    void End(ID3D12GraphicsCommandList* cmd)
    {
        if (m_Active < 0) return;
        const UINT first = UINT(m_Active) * 2;
        cmd->EndQuery(m_Heap, D3D12_QUERY_TYPE_TIMESTAMP, first + 1);
        cmd->ResolveQueryData(m_Heap, D3D12_QUERY_TYPE_TIMESTAMP, first, 2, m_Readback, UINT64(first) * sizeof(UINT64));
        m_Pending[m_Active] = true;
        m_Active            = -1;
    }
};

} // namespace Benchmark

#pragma once

#include <nvrhi/nvrhi.h>

#include <d3d12.h>

#include <vector>

// Asteroids.exe: pipeline statistics query (0x30 bytes; ctor 0x140040C10 "PipelineStatsQueryResult", owned by
// FeatureDemo+328 with 3 slots: 0 = shadows, 1 = G-buffer, 2 = particles).
// Begin 0x140041170, End 0x1400411C0, Resolve 0x140044900 (end of the frame), ReadResults 0x140041260 (start of
// the next frame; FeatureDemo shows results[1].CInvocations (+ results[2] when particles are on) as the number of
// rasterized primitives).
//
// deviation: nvrhi has no pipeline statistics queries (only timer / event queries). As in 2018, the query heap
// and the Begin/End/Resolve calls go straight to D3D12 through nvrhi's native objects
// (ObjectTypes::D3D12_Device, D3D12_GraphicsCommandList, D3D12_Resource); the readback buffer is an nvrhi buffer.
// The 2018 code read the results without waiting for the GPU; this reconstruction does the same.
class PipelineStatisticsQuery
{
public:
    PipelineStatisticsQuery(nvrhi::IDevice* device, uint32_t numSlots);

    void Begin(nvrhi::ICommandList* commandList, uint32_t slot) const;
    void End(nvrhi::ICommandList* commandList, uint32_t slot) const;

    // Resolves all slots into the readback buffer. Call outside of any Begin/End pair.
    void Resolve(nvrhi::ICommandList* commandList) const;

    // Copies the readback buffer into GetResults().
    void ReadResults();

    [[nodiscard]] const std::vector<D3D12_QUERY_DATA_PIPELINE_STATISTICS>& GetResults() const { return m_Results; }
    [[nodiscard]] uint32_t GetNumSlots() const { return m_NumSlots; }
    [[nodiscard]] bool IsValid() const { return m_QueryHeap != nullptr && m_ReadbackBuffer != nullptr; }

private:
    nvrhi::DeviceHandle m_Device;                               // +0
    nvrhi::BufferHandle m_ReadbackBuffer;                       // +8  "PipelineStatsQueryResult" (88 bytes per slot)
    nvrhi::AutoPtr<ID3D12QueryHeap> m_QueryHeap;            // +16 D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS
    std::vector<D3D12_QUERY_DATA_PIPELINE_STATISTICS> m_Results; // +24
    uint32_t m_NumSlots = 0;
};

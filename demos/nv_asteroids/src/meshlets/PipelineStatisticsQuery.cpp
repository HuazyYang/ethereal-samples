#include "meshlets/PipelineStatisticsQuery.h"

#include <donut/core/log.h>

#include <cstring>

using namespace donut;

namespace
{
    ID3D12GraphicsCommandList* GetNativeCommandList(nvrhi::ICommandList* commandList)
    {
        return commandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList);
    }
}

PipelineStatisticsQuery::PipelineStatisticsQuery(nvrhi::IDevice* device, uint32_t numSlots)
    : m_Device(device)
    , m_NumSlots(numSlots)
{
    // Asteroids.exe: 0x140040C10
    ID3D12Device* d3dDevice = device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);
    if (!d3dDevice)
    {
        log::warning("PipelineStatisticsQuery requires the D3D12 backend");
        return;
    }

    D3D12_QUERY_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
    heapDesc.Count = numSlots;
    heapDesc.NodeMask = 0;
    if (FAILED(d3dDevice->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_QueryHeap))))
    {
        log::error("Failed to create the pipeline statistics query heap");
        m_QueryHeap = nullptr;
    }

    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) * numSlots;
    bufferDesc.debugName = "PipelineStatsQueryResult";
    bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
    bufferDesc.initialState = nvrhi::ResourceStates::CopyDest;
    bufferDesc.keepInitialState = true;
    m_ReadbackBuffer = device->createBuffer(bufferDesc);

    m_Results.resize(numSlots);
    std::memset(m_Results.data(), 0, sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) * numSlots);
}

void PipelineStatisticsQuery::Begin(nvrhi::ICommandList* commandList, uint32_t slot) const
{
    // Asteroids.exe: 0x140041170
    if (!IsValid() || slot >= m_NumSlots)
        return;

    if (ID3D12GraphicsCommandList* d3dCommandList = GetNativeCommandList(commandList))
        d3dCommandList->BeginQuery(m_QueryHeap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, slot);
}

void PipelineStatisticsQuery::End(nvrhi::ICommandList* commandList, uint32_t slot) const
{
    // Asteroids.exe: 0x1400411C0
    if (!IsValid() || slot >= m_NumSlots)
        return;

    if (ID3D12GraphicsCommandList* d3dCommandList = GetNativeCommandList(commandList))
        d3dCommandList->EndQuery(m_QueryHeap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, slot);
}

void PipelineStatisticsQuery::Resolve(nvrhi::ICommandList* commandList) const
{
    // Asteroids.exe: 0x140044900
    if (!IsValid())
        return;

    // deviation: nvrhi tracks the resource states; make sure the readback buffer is a copy destination
    // before writing to it behind nvrhi's back.
    commandList->setBufferState(m_ReadbackBuffer, nvrhi::ResourceStates::CopyDest);
    commandList->commitBarriers();

    ID3D12GraphicsCommandList* d3dCommandList = GetNativeCommandList(commandList);
    ID3D12Resource* d3dBuffer = m_ReadbackBuffer->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
    if (d3dCommandList && d3dBuffer)
        d3dCommandList->ResolveQueryData(m_QueryHeap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, m_NumSlots, d3dBuffer, 0);
}

void PipelineStatisticsQuery::ReadResults()
{
    // Asteroids.exe: 0x140041260
    if (!IsValid())
        return;

    const void* data = m_Device->mapBuffer(m_ReadbackBuffer, nvrhi::CpuAccessMode::Read);
    if (!data)
        return;

    std::memcpy(m_Results.data(), data, sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) * m_Results.size());
    m_Device->unmapBuffer(m_ReadbackBuffer);
}

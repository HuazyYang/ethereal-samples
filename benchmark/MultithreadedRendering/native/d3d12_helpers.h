#pragma once
// Small D3D12 helpers of the native renderer: error handling, resource descriptors, and the
// UploadBufferStack of D3D12-Trial/Common (the per-frame linear allocator the sample pushes its
// constant buffers through from several threads).

#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "benchmark.h"

namespace MtNative
{

using Microsoft::WRL::ComPtr;

// Fatal in benchmark mode (writes the failure record), an exception otherwise.
inline void ThrowIfFailed(HRESULT hr, const char* what)
{
    if (SUCCEEDED(hr)) return;
    char text[256];
    snprintf(text, sizeof(text), "%s failed with HRESULT 0x%08lx", what, static_cast<unsigned long>(hr));
    if (Benchmark::config.enabled) Benchmark::Fail(text);
    throw std::runtime_error(text);
}

inline constexpr uint32_t AlignUp(uint32_t value, uint32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }
inline constexpr uint64_t AlignUp64(uint64_t value, uint64_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

inline D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES props = {};
    props.Type = type;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    props.CreationNodeMask = 1;
    props.VisibleNodeMask = 1;
    return props;
}

inline D3D12_RESOURCE_DESC BufferDesc(uint64_t size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

inline D3D12_RESOURCE_DESC Tex2DDesc(DXGI_FORMAT format, uint32_t width, uint32_t height, uint16_t mipLevels,
                                     D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = mipLevels;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
}

inline D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

inline D3D12_CPU_DESCRIPTOR_HANDLE Offset(D3D12_CPU_DESCRIPTOR_HANDLE h, uint32_t index, uint32_t increment)
{
    h.ptr += SIZE_T(index) * increment;
    return h;
}
inline D3D12_GPU_DESCRIPTOR_HANDLE Offset(D3D12_GPU_DESCRIPTOR_HANDLE h, uint32_t index, uint32_t increment)
{
    h.ptr += UINT64(index) * increment;
    return h;
}

// Linear allocator of 256-byte aligned constant buffer slices in upload-heap blocks; one per frame
// in flight, shared by every recording thread of that frame (a lock per Push, as the sample's).
class UploadBufferStack
{
public:
    void Initialize(ID3D12Device* device, uint32_t blockSize)
    {
        m_Device = device;
        m_BlockSize = AlignUp(blockSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        Clear();
    }

    // Copies `size` bytes into a fresh slice and returns its GPU address.
    D3D12_GPU_VIRTUAL_ADDRESS Push(const void* data, uint32_t size)
    {
        const uint32_t alignedSize = AlignUp(size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        if (alignedSize > m_BlockSize)
            ThrowIfFailed(E_INVALIDARG, "UploadBufferStack::Push: slice larger than a block");

        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_CurrentBlock < 0 || m_Offset + alignedSize > m_BlockSize)
        {
            ++m_CurrentBlock;
            m_Offset = 0;
            if (size_t(m_CurrentBlock) >= m_Blocks.size())
                AllocateBlock();
        }
        Block& block = m_Blocks[size_t(m_CurrentBlock)];
        memcpy(block.mapped + m_Offset, data, size);
        const D3D12_GPU_VIRTUAL_ADDRESS address = block.gpuAddress + m_Offset;
        m_Offset += alignedSize;
        return address;
    }

    // Rewinds to the first block (the GPU must be done with the frame).
    void Clear()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_CurrentBlock = -1;
        m_Offset = 0;
    }

    size_t BlockCount() const { return m_Blocks.size(); }

private:
    struct Block
    {
        ComPtr<ID3D12Resource> resource;
        uint8_t* mapped = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = 0;
    };

    void AllocateBlock()
    {
        Block block;
        const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC desc = BufferDesc(m_BlockSize);
        ThrowIfFailed(m_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&block.resource)),
                      "CreateCommittedResource (constant buffer block)");
        ThrowIfFailed(block.resource->Map(0, nullptr, reinterpret_cast<void**>(&block.mapped)), "Map (constant buffer block)");
        block.gpuAddress = block.resource->GetGPUVirtualAddress();
        m_Blocks.push_back(std::move(block));
    }

    ID3D12Device* m_Device = nullptr;
    uint32_t m_BlockSize = 0;
    std::vector<Block> m_Blocks;
    int m_CurrentBlock = -1;
    uint32_t m_Offset = 0;
    std::mutex m_Mutex;
};

} // namespace MtNative

#pragma once

// Asteroids.exe: HBAO+ wrapper (40 bytes at FeatureDemo+896, no RTTI)
//   ctor 0x140096F40, dtor 0x140096F90, context release 0x140097170,
//   context creation 0x140097000 (GFSDK_SSAO_CreateContext_D3D12, header version {4,0,0,24284062}),
//   Render 0x140097230 ("HBAO+" marker).
//
// The 2018 wrapper allocated 60 CBV/SRV/UAV and 40 RTV descriptors from nvrhi's own D3D12 descriptor heaps and
// handed those ranges to HBAO+, so that the SRV GPU handles nvrhi returns for the depth and normal textures are
// valid in the heap HBAO+ binds. The same is done here through nvrhi::d3d12::IDevice::getDescriptorHeap. When
// nvrhi replaces a heap (it grows on demand) the context is recreated, as in 2018.

#include <nvrhi/nvrhi.h>

#include <cstdint>

struct GFSDK_SSAO_Context_D3D12;
struct HbaoParameters;

namespace donut::engine
{
    class ICompositeView;
}

class HbaoPlusPass
{
public:
    explicit HbaoPlusPass(nvrhi::IDevice* device);      // 0x140096F40
    ~HbaoPlusPass();                                    // 0x140096F90
    HbaoPlusPass(const HbaoPlusPass&) = delete;
    HbaoPlusPass& operator=(const HbaoPlusPass&) = delete;

    bool IsValid() const { return m_Context != nullptr; }

    // 0x140097230: renders HBAO+ for every planar child view and multiplies it into 'output'.
    // 'depth' is targets[0], 'normals' (world-space GBuffer2, may be null) targets[4], 'output' targets[5].
    void Render(nvrhi::ICommandList* commandList, const HbaoParameters& params,
        const donut::engine::ICompositeView& compositeView,
        nvrhi::ITexture* depth, nvrhi::ITexture* normals, nvrhi::ITexture* output);

private:
    void CreateContext();                               // 0x140097000
    void ReleaseContext();                              // 0x140097170
    void* GetCurrentSrvHeap() const;
    void* GetCurrentRtvHeap() const;

    nvrhi::DeviceHandle m_Device;                       // +0
    GFSDK_SSAO_Context_D3D12* m_Context = nullptr;      // +8
    uint32_t m_SrvBaseIndex = 0;                        // +16
    uint32_t m_RtvBaseIndex = 0;                        // +20
    void* m_SrvHeap = nullptr;                          // +24 ID3D12DescriptorHeap* the range was allocated from
    void* m_RtvHeap = nullptr;                          // +32
    bool m_HasRanges = false;
};

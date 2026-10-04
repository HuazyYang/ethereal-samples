#pragma once

// Asteroids.exe: render target set of FeatureDemo (+768), constructor 0x14004A2C0 (304 bytes, no RTTI),
// IsUpdateRequired 0x14004B3C0, Clear 0x14004B270. Created by the view/target setup 0x140031EF0 as
// RenderTargets(device, size, UNKNOWN, 0, false).
//
// The 2018 framebuffers were FramebufferFactory objects too; they are donut FramebufferFactory here.
// Formats are translated from the 2018 nvrhi enum (d3d12 table at 0x140270BE8 -> DXGI) to donut main's enum.

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <memory>

namespace donut::engine
{
    class FramebufferFactory;
}

class RenderTargets
{
public:
    nvrhi::TextureHandle DepthBuffer;                   // [0]  D24S8, cleared to 1.0
    nvrhi::TextureHandle HiZTexture;                    // [1]  R32_FLOAT, 5 mips, (size+127)/8 aligned down to 16
    nvrhi::TextureHandle GBuffer0;                      // [2]  SRGBA8_UNORM
    nvrhi::TextureHandle GBuffer1;                      // [3]  SRGBA8_UNORM
    nvrhi::TextureHandle GBuffer2;                      // [4]  RGBA16_SNORM
    nvrhi::TextureHandle HdrColor;                      // [5]  RGBA16_FLOAT
    nvrhi::TextureHandle LdrColor;                      // [6]  SRGBA8_UNORM
    nvrhi::TextureHandle MotionVectors;                 // [7]  RG16_FLOAT
    nvrhi::TextureHandle UnusedTexture8;                // [8]  never created by the binary
    nvrhi::TextureHandle DumpTexture;                   // [9]  optional array texture for frame dumps
    nvrhi::TextureHandle ResolvedColor1;                // [10] RGBA16_FLOAT, UAV (TAA ping-pong)
    nvrhi::TextureHandle ResolvedColor2;                // [11]
    nvrhi::TextureHandle BloomColor;                    // [12] RGBA16_FLOAT
    nvrhi::TextureHandle AccumulationRenderingBuffer;   // [13] RGBA16_FLOAT array of 64 slices (optional)
    nvrhi::StagingTextureHandle DumpStagingTexture;     // [14] CPU-readable copy of DumpTexture
    // deviation: donut's TemporalAntiAliasingPass writes a resolved texture and ping-pongs two separate feedback
    // textures internally; the 2018 TAA resolved into ResolvedColor1/2 and FeatureDemo swapped them each frame.
    // ResolvedColor1 is the resolved output, ResolvedColor2 and this texture are the feedback pair.
    nvrhi::TextureHandle TemporalFeedback;

    std::shared_ptr<donut::engine::FramebufferFactory> HdrFramebuffer;          // [15] HdrColor + depth
    std::shared_ptr<donut::engine::FramebufferFactory> HdrFramebufferNoDepth;   // [17] HdrColor
    std::shared_ptr<donut::engine::FramebufferFactory> GBufferFramebuffer;      // [19] GBuffer0..2 + MotionVectors + depth
    std::shared_ptr<donut::engine::FramebufferFactory> LdrFramebuffer;          // [21] LdrColor
    std::shared_ptr<donut::engine::FramebufferFactory> ResolvedFramebuffer1;    // [23] ResolvedColor1
    std::shared_ptr<donut::engine::FramebufferFactory> ResolvedFramebuffer2;    // [25] ResolvedColor2
    std::shared_ptr<donut::engine::FramebufferFactory> BloomFramebuffer;        // [27] BloomColor
    std::shared_ptr<donut::engine::FramebufferFactory> BloomFramebufferWithDepth; // [29] BloomColor + depth
    std::shared_ptr<donut::engine::FramebufferFactory> UnusedFramebuffer31;     // [31] never created by the binary
    std::shared_ptr<donut::engine::FramebufferFactory> DumpFramebuffer;         // [33] DumpTexture (optional)
    std::shared_ptr<donut::engine::FramebufferFactory> AccumulationFramebuffer; // [35] AccumulationRenderingBuffer (optional)

    // 'dumpFormat' != UNKNOWN creates DumpTexture (an array of 'dumpArraySize' slices) with a staging copy;
    // 'enableAccumulation' creates AccumulationRenderingBuffer.
    RenderTargets(nvrhi::IDevice* device, dm::uint2 size, nvrhi::Format dumpFormat = nvrhi::Format::UNKNOWN,
        uint32_t dumpArraySize = 0, bool enableAccumulation = false);

    // 0x14004B3C0
    bool IsUpdateRequired(dm::uint2 size) const { return any(m_Size != size); }

    // 0x14004B270: clears depth (1.0), LdrColor, HdrColor, GBuffer0..2 and MotionVectors (0).
    void Clear(nvrhi::ICommandList* commandList) const;

    dm::uint2 GetSize() const { return m_Size; }

private:
    dm::uint2 m_Size;                                   // [37] (+296)
};

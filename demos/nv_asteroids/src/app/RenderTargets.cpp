#include "app/RenderTargets.h"

#include <donut/engine/FramebufferFactory.h>

using namespace donut::math;
using namespace donut::engine;

namespace
{
    std::shared_ptr<FramebufferFactory> MakeFramebuffer(nvrhi::IDevice* device,
        std::initializer_list<nvrhi::ITexture*> colorTargets, nvrhi::ITexture* depthTarget)
    {
        auto factory = std::make_shared<FramebufferFactory>(device);
        for (nvrhi::ITexture* texture : colorTargets)
            factory->RenderTargets.push_back(texture);
        factory->DepthTarget = depthTarget;
        return factory;
    }
}

RenderTargets::RenderTargets(nvrhi::IDevice* device, uint2 size, nvrhi::Format dumpFormat,
    uint32_t dumpArraySize, bool enableAccumulation)
    : m_Size(size)
{
    // The 2018 code reuses one descriptor and mutates it between the createTexture calls;
    // the sequence below keeps the same order and the same inherited fields.
    nvrhi::TextureDesc desc;
    desc.width = size.x;
    desc.height = size.y;
    desc.depth = 1;
    desc.arraySize = 1;
    desc.mipLevels = 1;
    desc.sampleCount = 1;
    desc.sampleQuality = 0;
    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.isRenderTarget = true;
    desc.isUAV = false;
    desc.isTypeless = true;
    desc.useClearValue = true;
    desc.keepInitialState = true;

    desc.format = nvrhi::Format::D24S8;                 // 2018 format 47
    desc.initialState = nvrhi::ResourceStates::DepthWrite;
    // deviation: the binary's clear value is 0 although Clear() clears depth to 1; use the matching value.
    desc.clearValue = nvrhi::Color(1.f);
    desc.debugName = "DepthBuffer";
    DepthBuffer = device->createTexture(desc);

    desc.isTypeless = false;
    desc.clearValue = nvrhi::Color(0.f);
    desc.format = nvrhi::Format::RGBA16_FLOAT;          // 2018 format 34
    desc.initialState = nvrhi::ResourceStates::RenderTarget;
    desc.debugName = "HdrColor";
    HdrColor = device->createTexture(desc);

    desc.format = nvrhi::Format::SRGBA8_UNORM;          // 2018 format 22
    desc.debugName = "GBuffer0";
    GBuffer0 = device->createTexture(desc);

    desc.format = nvrhi::Format::SRGBA8_UNORM;
    desc.debugName = "GBuffer1";
    GBuffer1 = device->createTexture(desc);

    desc.format = nvrhi::Format::RGBA16_SNORM;          // 2018 format 36
    desc.debugName = "GBuffer2";
    GBuffer2 = device->createTexture(desc);

    desc.format = nvrhi::Format::RG16_FLOAT;            // 2018 format 30
    desc.debugName = "MotionVectors";
    MotionVectors = device->createTexture(desc);

    if (dumpFormat != nvrhi::Format::UNKNOWN)
    {
        nvrhi::TextureDesc dumpDesc = desc;
        dumpDesc.format = dumpFormat;
        dumpDesc.isUAV = false;
        dumpDesc.debugName = "DumpTexture";
        dumpDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
        dumpDesc.arraySize = dumpArraySize;
        DumpTexture = device->createTexture(dumpDesc);
        DumpStagingTexture = device->createStagingTexture(dumpDesc, nvrhi::CpuAccessMode::Read);
    }

    if (enableAccumulation)
    {
        nvrhi::TextureDesc accumulationDesc = desc;
        accumulationDesc.format = nvrhi::Format::RGBA16_FLOAT;
        accumulationDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
        accumulationDesc.arraySize = 64;
        accumulationDesc.debugName = "AccumulationRenderingBuffer";
        AccumulationRenderingBuffer = device->createTexture(accumulationDesc);
    }

    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.arraySize = 1;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.isUAV = true;
    desc.debugName = "ResolvedColor1";
    ResolvedColor1 = device->createTexture(desc);

    desc.debugName = "ResolvedColor2";
    ResolvedColor2 = device->createTexture(desc);

    desc.debugName = "TemporalFeedback";
    TemporalFeedback = device->createTexture(desc);

    desc.isUAV = false;
    desc.debugName = "BloomColor";
    BloomColor = device->createTexture(desc);

    desc.format = nvrhi::Format::SRGBA8_UNORM;
    desc.isUAV = false;
    desc.debugName = "LdrColor";
    LdrColor = device->createTexture(desc);

    desc.width = ((size.x + 127) >> 3) & 0x1FFFFFF0u;
    desc.height = ((size.y + 127) >> 3) & 0x1FFFFFF0u;
    desc.mipLevels = 5;
    desc.isUAV = true;
    desc.isTypeless = false; // unresolved: the binary sets a third flag byte here; plain R32_FLOAT views are used
    desc.format = nvrhi::Format::R32_FLOAT;             // 2018 format 33
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.debugName = "HiZTexture";
    HiZTexture = device->createTexture(desc);

    HdrFramebuffer = MakeFramebuffer(device, { HdrColor }, DepthBuffer);
    GBufferFramebuffer = MakeFramebuffer(device, { GBuffer0, GBuffer1, GBuffer2, MotionVectors }, DepthBuffer);
    HdrFramebufferNoDepth = MakeFramebuffer(device, { HdrColor }, nullptr);
    LdrFramebuffer = MakeFramebuffer(device, { LdrColor }, nullptr);
    ResolvedFramebuffer1 = MakeFramebuffer(device, { ResolvedColor1 }, nullptr);
    ResolvedFramebuffer2 = MakeFramebuffer(device, { ResolvedColor2 }, nullptr);
    BloomFramebuffer = MakeFramebuffer(device, { BloomColor }, nullptr);
    BloomFramebufferWithDepth = MakeFramebuffer(device, { BloomColor }, DepthBuffer);

    if (dumpFormat != nvrhi::Format::UNKNOWN)
        DumpFramebuffer = MakeFramebuffer(device, { DumpTexture }, nullptr);

    if (enableAccumulation)
        AccumulationFramebuffer = MakeFramebuffer(device, { AccumulationRenderingBuffer }, nullptr);
}

void RenderTargets::Clear(nvrhi::ICommandList* commandList) const
{
    commandList->clearDepthStencilTexture(DepthBuffer, nvrhi::AllSubresources, true, 1.f, true, 0);
    commandList->clearTextureFloat(LdrColor, nvrhi::AllSubresources, nvrhi::Color(0.f));
    commandList->clearTextureFloat(HdrColor, nvrhi::AllSubresources, nvrhi::Color(0.f));
    commandList->clearTextureFloat(GBuffer0, nvrhi::AllSubresources, nvrhi::Color(0.f));
    commandList->clearTextureFloat(GBuffer1, nvrhi::AllSubresources, nvrhi::Color(0.f));
    commandList->clearTextureFloat(GBuffer2, nvrhi::AllSubresources, nvrhi::Color(0.f));
    commandList->clearTextureFloat(MotionVectors, nvrhi::AllSubresources, nvrhi::Color(0.f));
}

#include "SampleRenderTargets.h"
#include <nvrhi/utils.h>

SampleRenderTargets::SampleRenderTargets(nvrhi::IDevice* device)
    : Device{device}, Size{0, 0} {}

bool SampleRenderTargets::Init(dm::uint2 size) {
    Size = size;

    nvrhi::TextureDesc desc;
    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.width = size.x;
    desc.height = size.y;
    desc.initialState = nvrhi::ResourceStates::RenderTarget;
    desc.isRenderTarget = true;
    desc.useClearValue = true;
    desc.clearValue = nvrhi::Color{0.f};
    desc.keepInitialState = true;
    desc.isTypeless = false;
    desc.isUAV = false;
    desc.mipLevels = 1;

    desc.format = nvrhi::Format::RGBA8_UNORM;
    desc.debugName = "GBufferDiffuse";
    GBufferDiffuse = Device->createTexture(desc);

    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.debugName = "GBufferNormals";
    GBufferNormals = Device->createTexture(desc);

    const nvrhi::Format depthFormats[] = {nvrhi::Format::D24S8, nvrhi::Format::D32S8, nvrhi::Format::D32,
                                          nvrhi::Format::D16};

    const nvrhi::FormatSupport depthFeatures =
        nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::DepthStencil | nvrhi::FormatSupport::ShaderLoad;

    desc.format = nvrhi::utils::ChooseFormat(Device, depthFeatures, depthFormats, std::size(depthFormats));
    desc.isTypeless = true;
    desc.initialState = nvrhi::ResourceStates::DepthWrite;
    desc.clearValue = nvrhi::Color{1.f};
    desc.debugName = "GBufferDepth";
    GBufferDepth = Device->createTexture(desc);

    GBufferFramebuffer = MAKE_RC_OBJ_PTR(donut::engine::FramebufferFactory, Device);
    GBufferFramebuffer->RenderTargets = {
        GBufferDiffuse,
        GBufferNormals
    };
    GBufferFramebuffer->DepthTarget = GBufferDepth;

    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.isTypeless = false;
    desc.clearValue = nvrhi::Color{0.f};
    desc.isUAV = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.debugName = "ShaderColor";
    HdrColorBuffer = Device->createTexture(desc);

    HdrFramebuffer = MAKE_RC_OBJ_PTR(donut::engine::FramebufferFactory, Device);
    HdrFramebuffer->RenderTargets = { HdrColorBuffer };

    if(!VoxelizationDummyFramebuffer) {
        VoxelizationDummyFramebuffer = MAKE_RC_OBJ_PTR(donut::engine::FramebufferFactory, Device);
    }

    return true;
}

void SampleRenderTargets::ClearGBuffers(nvrhi::ICommandList* commandList) {
    const auto& depthFormatInfo = nvrhi::getFormatInfo(GBufferDepth->getDesc().format);

    commandList->clearDepthStencilTexture(GBufferDepth, nvrhi::AllSubresources, true, 1.f,
                                          depthFormatInfo.hasStencil, 0);
    commandList->clearTextureFloat(GBufferDiffuse, nvrhi::AllSubresources, nvrhi::Color{0.f});
    commandList->clearTextureFloat(GBufferNormals, nvrhi::AllSubresources, nvrhi::Color{0.f});
}

void SampleRenderTargets::CreateShadowFramebuffer(nvrhi::ITexture* shadowMapTexture) {
    ShadowMapTexture = shadowMapTexture;
    ShadowMapFramebuffer = MAKE_RC_OBJ_PTR(donut::engine::FramebufferFactory, Device);
    ShadowMapFramebuffer->DepthTarget = ShadowMapTexture;
}

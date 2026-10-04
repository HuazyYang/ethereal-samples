#include "passes/LightProbeProcessingPass2018.h"
#include "app/GpuProfiler.h"

#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/ShaderFactory.h>

#include <algorithm>
#include <cassert>
#include <cmath>

#include "passes/Framework2018Constants.h"

using namespace donut::engine;

namespace
{
    const char* const c_LightProbeShader = "framework/passes/light_probe.hlsl";
}

LightProbeProcessingPass2018::LightProbeProcessingPass2018(
    nvrhi::IDevice* device,
    std::shared_ptr<ShaderFactory> shaderFactory,
    std::shared_ptr<CommonRenderPasses> commonPasses,
    uint32_t intermediateTextureSize,
    nvrhi::Format intermediateTextureFormat)
    : m_Device(device)
    , m_IntermediateTextureSize(intermediateTextureSize)
    , m_CommonPasses(std::move(commonPasses))
{
    m_GeometryShader = shaderFactory->CreateShader(c_LightProbeShader, "cubemap_gs", nullptr, nvrhi::ShaderType::Geometry);
    m_MipPixelShader = shaderFactory->CreateShader(c_LightProbeShader, "mip_ps", nullptr, nvrhi::ShaderType::Pixel);
    m_DiffusePixelShader = shaderFactory->CreateShader(c_LightProbeShader, "diffuse_probe_ps", nullptr,
        nvrhi::ShaderType::Pixel);
    m_SpecularPixelShader = shaderFactory->CreateShader(c_LightProbeShader, "specular_probe_ps", nullptr,
        nvrhi::ShaderType::Pixel);
    m_EnvironmentBrdfPixelShader = shaderFactory->CreateShader(c_LightProbeShader, "environment_brdf_ps", nullptr,
        nvrhi::ShaderType::Pixel);

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::Sampler(0),
        nvrhi::BindingLayoutItem::Texture_SRV(0),
    };
    m_BindingLayout = device->createBindingLayout(layoutDesc);

    nvrhi::BufferDesc constantBufferDesc;
    constantBufferDesc.byteSize = sizeof(framework2018::LightProbeConstants);
    constantBufferDesc.debugName = "SsaoConstants"; // sic, Asteroids.exe 0x14008B030
    constantBufferDesc.isConstantBuffer = true;
    constantBufferDesc.isVolatile = true;
    constantBufferDesc.maxVersions = 64;
    m_LightProbeCB = device->createBuffer(constantBufferDesc);

    assert(intermediateTextureSize > 0);

    nvrhi::TextureDesc cubemapDesc;
    cubemapDesc.arraySize = 6;
    cubemapDesc.width = intermediateTextureSize;
    cubemapDesc.height = intermediateTextureSize;
    cubemapDesc.mipLevels = uint32_t(std::floor(std::log2(float(intermediateTextureSize)))) + 1;
    cubemapDesc.dimension = nvrhi::TextureDimension::TextureCube;
    cubemapDesc.isRenderTarget = true;
    cubemapDesc.format = intermediateTextureFormat;
    cubemapDesc.initialState = nvrhi::ResourceStates::RenderTarget;
    cubemapDesc.keepInitialState = true;
    cubemapDesc.clearValue = nvrhi::Color(0.f);
    cubemapDesc.useClearValue = true;
    cubemapDesc.debugName = "LightProbeIntermediate";
    m_IntermediateTexture = device->createTexture(cubemapDesc);

    // 2018 format 30 = RG16_FLOAT in the 2018 nvrhi enumeration.
    nvrhi::TextureDesc brdfTextureDesc;
    brdfTextureDesc.width = m_EnvironmentBrdfTextureSize;
    brdfTextureDesc.height = m_EnvironmentBrdfTextureSize;
    brdfTextureDesc.format = nvrhi::Format::RG16_FLOAT;
    brdfTextureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    brdfTextureDesc.keepInitialState = true;
    brdfTextureDesc.isRenderTarget = true;
    brdfTextureDesc.clearValue = nvrhi::Color(0.f);
    brdfTextureDesc.useClearValue = true;
    brdfTextureDesc.debugName = "EnvironmentBrdf";
    m_EnvironmentBrdfTexture = device->createTexture(brdfTextureDesc);
}

nvrhi::FramebufferHandle LightProbeProcessingPass2018::GetCachedFramebuffer(nvrhi::ITexture* texture,
    nvrhi::TextureSubresourceSet subresources)
{
    nvrhi::FramebufferHandle& framebuffer = m_FramebufferCache[TextureSubresourcesKey{ texture, subresources }];
    if (!framebuffer)
        framebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture, subresources));
    return framebuffer;
}

nvrhi::BindingSetHandle LightProbeProcessingPass2018::GetCachedBindingSet(nvrhi::ITexture* texture,
    nvrhi::TextureSubresourceSet subresources)
{
    nvrhi::BindingSetHandle& bindingSet = m_BindingSetCache[TextureSubresourcesKey{ texture, subresources }];
    if (!bindingSet)
    {
        // 0x14008C240: CommonRenderPasses+304 (linear wrap sampler).
        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_LightProbeCB),
            nvrhi::BindingSetItem::Sampler(0, m_CommonPasses->m_LinearWrapSampler),
            nvrhi::BindingSetItem::Texture_SRV(0, texture, nvrhi::Format::UNKNOWN, subresources,
                nvrhi::TextureDimension::TextureCube),
        };
        bindingSet = m_Device->createBindingSet(bindingSetDesc, m_BindingLayout);
    }
    return bindingSet;
}

nvrhi::GraphicsPipelineHandle LightProbeProcessingPass2018::GetPipeline(
    std::unordered_map<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle>& cache, nvrhi::IShader* pixelShader,
    nvrhi::IFramebuffer* framebuffer)
{
    nvrhi::GraphicsPipelineHandle& pso = cache[framebuffer->getFramebufferInfo()];
    if (!pso)
    {
        nvrhi::GraphicsPipelineDesc psoDesc;
        psoDesc.VS = m_CommonPasses->m_FullscreenVS;
        psoDesc.GS = m_GeometryShader;
        psoDesc.PS = pixelShader;
        psoDesc.bindingLayouts = { m_BindingLayout };
        psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        psoDesc.renderState.rasterState.setCullNone();
        psoDesc.renderState.depthStencilState.depthTestEnable = false;
        psoDesc.renderState.depthStencilState.stencilEnable = false;
        pso = m_Device->createGraphicsPipeline(psoDesc, framebuffer);
    }
    return pso;
}

void LightProbeProcessingPass2018::DrawCubeFaces(nvrhi::ICommandList* commandList, nvrhi::IGraphicsPipeline* pipeline,
    nvrhi::IFramebuffer* framebuffer, nvrhi::IBindingSet* bindingSet, float size)
{
    nvrhi::GraphicsState state;
    state.pipeline = pipeline;
    state.framebuffer = framebuffer;
    state.bindings = { bindingSet };
    state.viewport.scissorRects = { nvrhi::Rect(int(size), int(size)) };
    state.viewport.viewports = { nvrhi::Viewport(size, size) };
    commandList->setGraphicsState(state);

    // One full-screen strip; cubemap_gs replicates it to the six faces (SV_RenderTargetArrayIndex).
    nvrhi::DrawArguments args;
    args.vertexCount = 4;
    args.instanceCount = 1;
    commandList->draw(args);
}

void LightProbeProcessingPass2018::BlitCubemap(nvrhi::ICommandList* commandList, nvrhi::ITexture* inCubeMap,
    uint32_t inBaseArraySlice, uint32_t inMipLevel, nvrhi::ITexture* outCubeMap, uint32_t outBaseArraySlice,
    uint32_t outMipLevel)
{
    // 0x14008BA80
    const nvrhi::TextureDesc& outputDesc = outCubeMap->getDesc();

    nvrhi::FramebufferHandle framebuffer = GetCachedFramebuffer(outCubeMap,
        nvrhi::TextureSubresourceSet(outMipLevel, 1, outBaseArraySlice, 6));
    nvrhi::GraphicsPipelineHandle pso = GetPipeline(m_BlitPsoCache, m_MipPixelShader, framebuffer);

    framework2018::LightProbeConstants constants = {};
    commandList->writeBuffer(m_LightProbeCB, &constants, sizeof(constants));

    nvrhi::BindingSetHandle bindingSet = GetCachedBindingSet(inCubeMap,
        nvrhi::TextureSubresourceSet(inMipLevel, 1, inBaseArraySlice, 6));

    const float mipSize = std::ceil(float(outputDesc.width) * std::pow(0.5f, float(outMipLevel)));
    DrawCubeFaces(commandList, pso, framebuffer, bindingSet, mipSize);
}

void LightProbeProcessingPass2018::GenerateCubemapMips(nvrhi::ICommandList* commandList, nvrhi::ITexture* cubeMap,
    uint32_t baseArraySlice, uint32_t sourceMipLevel, uint32_t levelsToGenerate)
{
    // 0x14008C190
    demo::ProfBegin(commandList, "Cubemap Mips");

    for (uint32_t index = 0; index < levelsToGenerate; index++)
    {
        const uint32_t mipLevel = sourceMipLevel + index;
        BlitCubemap(commandList, cubeMap, baseArraySlice, mipLevel, cubeMap, baseArraySlice, mipLevel + 1);
    }

    demo::ProfEnd(commandList);
}

void LightProbeProcessingPass2018::RenderDiffuseMap(nvrhi::ICommandList* commandList,
    nvrhi::ITexture* inEnvironmentMap, nvrhi::TextureSubresourceSet inSubresources, nvrhi::ITexture* outDiffuseMap,
    uint32_t outBaseArraySlice, uint32_t outMipLevel)
{
    // 0x14008C7F0
    const nvrhi::TextureDesc& inDesc = inEnvironmentMap->getDesc();
    const float inputSize = std::ceil(float(inDesc.width) * std::pow(0.5f, float(inSubresources.baseMipLevel)));

    const nvrhi::TextureDesc& outDesc = outDiffuseMap->getDesc();
    const float outputSize = std::ceil(float(outDesc.width) * std::pow(0.5f, float(outMipLevel)));

    const uint32_t intermediateMipLevel = uint32_t(std::max(0.f,
        std::log2(float(m_IntermediateTextureSize) / outputSize) - 2.f));
    const float intermediateSize = std::ceil(float(m_IntermediateTextureSize)
        * std::pow(0.5f, float(intermediateMipLevel)));

    demo::ProfBegin(commandList, "Diffuse Light Probe");

    nvrhi::FramebufferHandle framebuffer = GetCachedFramebuffer(m_IntermediateTexture,
        nvrhi::TextureSubresourceSet(intermediateMipLevel, 1, 0, 6));
    nvrhi::GraphicsPipelineHandle pso = GetPipeline(m_DiffusePsoCache, m_DiffusePixelShader, framebuffer);

    framework2018::LightProbeConstants constants = {};
    constants.sampleCount = 4096;
    constants.lodBias = 1.0f + 0.5f * std::log2((inputSize * inputSize) / float(constants.sampleCount));
    commandList->writeBuffer(m_LightProbeCB, &constants, sizeof(constants));

    nvrhi::BindingSetHandle bindingSet = GetCachedBindingSet(inEnvironmentMap, inSubresources);
    DrawCubeFaces(commandList, pso, framebuffer, bindingSet, intermediateSize);

    BlitCubemap(commandList, m_IntermediateTexture, 0, intermediateMipLevel, m_IntermediateTexture, 0,
        intermediateMipLevel + 1);
    BlitCubemap(commandList, m_IntermediateTexture, 0, intermediateMipLevel + 1, outDiffuseMap, outBaseArraySlice,
        outMipLevel);

    demo::ProfEnd(commandList);
}

void LightProbeProcessingPass2018::RenderSpecularMap(nvrhi::ICommandList* commandList, float roughness,
    nvrhi::ITexture* inEnvironmentMap, nvrhi::TextureSubresourceSet inSubresources, nvrhi::ITexture* outSpecularMap,
    uint32_t outBaseArraySlice, uint32_t outMipLevel)
{
    // 0x14008D460
    const nvrhi::TextureDesc& inDesc = inEnvironmentMap->getDesc();
    const float inputSize = std::ceil(float(inDesc.width) * std::pow(0.5f, float(inSubresources.baseMipLevel)));

    const nvrhi::TextureDesc& outDesc = outSpecularMap->getDesc();
    const float outputSize = std::ceil(float(outDesc.width) * std::pow(0.5f, float(outMipLevel)));

    const uint32_t intermediateMipLevel = uint32_t(std::max(0.f,
        std::log2(float(m_IntermediateTextureSize) / outputSize) - 2.f));
    const float intermediateSize = std::ceil(float(m_IntermediateTextureSize)
        * std::pow(0.5f, float(intermediateMipLevel)));

    demo::ProfBegin(commandList, "Specular Light Probe");

    nvrhi::FramebufferHandle framebuffer = GetCachedFramebuffer(m_IntermediateTexture,
        nvrhi::TextureSubresourceSet(intermediateMipLevel, 1, 0, 6));
    nvrhi::GraphicsPipelineHandle pso = GetPipeline(m_SpecularPsoCache, m_SpecularPixelShader, framebuffer);

    framework2018::LightProbeConstants constants = {};
    constants.sampleCount = 1024;
    constants.lodBias = 1.0f;
    constants.roughness = std::max(0.01f, roughness);
    constants.inputCubeSize = inputSize;
    commandList->writeBuffer(m_LightProbeCB, &constants, sizeof(constants));

    nvrhi::BindingSetHandle bindingSet = GetCachedBindingSet(inEnvironmentMap, inSubresources);
    DrawCubeFaces(commandList, pso, framebuffer, bindingSet, intermediateSize);

    BlitCubemap(commandList, m_IntermediateTexture, 0, intermediateMipLevel, m_IntermediateTexture, 0,
        intermediateMipLevel + 1);
    BlitCubemap(commandList, m_IntermediateTexture, 0, intermediateMipLevel + 1, outSpecularMap, outBaseArraySlice,
        outMipLevel);

    demo::ProfEnd(commandList);
}

void LightProbeProcessingPass2018::RenderEnvironmentBrdfTexture(nvrhi::ICommandList* commandList)
{
    // 0x14008D020: no bindings, no geometry shader.
    demo::ProfBegin(commandList, "Environment BRDF");

    nvrhi::FramebufferHandle framebuffer = m_Device->createFramebuffer(
        nvrhi::FramebufferDesc().addColorAttachment(m_EnvironmentBrdfTexture));

    nvrhi::GraphicsPipelineDesc psoDesc;
    psoDesc.VS = m_CommonPasses->m_FullscreenVS;
    psoDesc.PS = m_EnvironmentBrdfPixelShader;
    psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
    psoDesc.renderState.rasterState.setCullNone();
    psoDesc.renderState.depthStencilState.depthTestEnable = false;
    psoDesc.renderState.depthStencilState.stencilEnable = false;
    nvrhi::GraphicsPipelineHandle pso = m_Device->createGraphicsPipeline(psoDesc, framebuffer);

    const float size = float(m_EnvironmentBrdfTextureSize);
    nvrhi::GraphicsState state;
    state.pipeline = pso;
    state.framebuffer = framebuffer;
    state.viewport.scissorRects = { nvrhi::Rect(int(size), int(size)) };
    state.viewport.viewports = { nvrhi::Viewport(size, size) };
    commandList->setGraphicsState(state);

    nvrhi::DrawArguments args;
    args.vertexCount = 4;
    args.instanceCount = 1;
    commandList->draw(args);

    demo::ProfEnd(commandList);
}

nvrhi::ITexture* LightProbeProcessingPass2018::GetEnvironmentBrdfTexture()
{
    return m_EnvironmentBrdfTexture;
}

void LightProbeProcessingPass2018::ResetCaches()
{
    m_BlitPsoCache.clear();
    m_DiffusePsoCache.clear();
    m_SpecularPsoCache.clear();
    m_FramebufferCache.clear();
    m_BindingSetCache.clear();
}

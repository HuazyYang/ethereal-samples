#pragma once

// Asteroids.exe: 2018 framework LightProbeProcessingPass (0x1C0-byte shared object; ctor 0x14008B030).
// FeatureDemo creates it lazily in RenderLightProbes (0x1400327F0, +864) with (1024, format 34 = RGBA16_FLOAT).
//
//   0x14008B030  ctor(device, shaderFactory, commonPasses, intermediateTextureSize, intermediateTextureFormat)
//   0x14008BA80  BlitCubemap        (mip_ps through cubemap_gs, zero constants)
//   0x14008C190  GenerateCubemapMips(commandList, cube, baseArraySlice, sourceMipLevel, levelsToGenerate)
//   0x14008C7F0  RenderDiffuseMap   (4096 samples, lodBias = 1 + 0.5 * log2(inputSize^2 / 4096))
//   0x14008D460  RenderSpecularMap  (1024 samples, lodBias 1, roughness = max(r, 0.01), inputCubeSize)
//   0x14008D020  RenderEnvironmentBrdfTexture (64 x 64 RG16_FLOAT "EnvironmentBrdf")
//   0x14008C7E0  GetEnvironmentBrdfTexture
//   0x14008C240 / 0x14008C610  cached binding sets / framebuffers (keyed by texture + subresources)
//
// The C++ is the same as the first public donut (2021) LightProbeProcessingPass; what differs from donut main is
// the shader file (framework/passes/light_probe.hlsl, reconstructed from the 2018 DXIL) and its constants
// (16 bytes, the buffer is named "SsaoConstants" in the binary - a copy/paste slip kept here).

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <memory>
#include <unordered_map>

namespace donut::engine
{
    class ShaderFactory;
    class CommonRenderPasses;
}

class LightProbeProcessingPass2018
{
public:
    LightProbeProcessingPass2018(
        nvrhi::IDevice* device,
        std::shared_ptr<donut::engine::ShaderFactory> shaderFactory,
        std::shared_ptr<donut::engine::CommonRenderPasses> commonPasses,
        uint32_t intermediateTextureSize = 1024,
        nvrhi::Format intermediateTextureFormat = nvrhi::Format::RGBA16_FLOAT);

    void BlitCubemap(nvrhi::ICommandList* commandList, nvrhi::ITexture* inCubeMap, uint32_t inBaseArraySlice,
        uint32_t inMipLevel, nvrhi::ITexture* outCubeMap, uint32_t outBaseArraySlice, uint32_t outMipLevel);

    void GenerateCubemapMips(nvrhi::ICommandList* commandList, nvrhi::ITexture* cubeMap, uint32_t baseArraySlice,
        uint32_t sourceMipLevel, uint32_t levelsToGenerate);

    void RenderDiffuseMap(nvrhi::ICommandList* commandList, nvrhi::ITexture* inEnvironmentMap,
        nvrhi::TextureSubresourceSet inSubresources, nvrhi::ITexture* outDiffuseMap, uint32_t outBaseArraySlice,
        uint32_t outMipLevel);

    void RenderSpecularMap(nvrhi::ICommandList* commandList, float roughness, nvrhi::ITexture* inEnvironmentMap,
        nvrhi::TextureSubresourceSet inSubresources, nvrhi::ITexture* outSpecularMap, uint32_t outBaseArraySlice,
        uint32_t outMipLevel);

    void RenderEnvironmentBrdfTexture(nvrhi::ICommandList* commandList);

    nvrhi::ITexture* GetEnvironmentBrdfTexture();

    void ResetCaches();

private:
    struct TextureSubresourcesKey
    {
        nvrhi::TextureHandle texture;
        nvrhi::TextureSubresourceSet subresources;

        bool operator==(const TextureSubresourcesKey& other) const
        {
            return texture == other.texture && subresources == other.subresources;
        }

        struct Hash
        {
            size_t operator()(const TextureSubresourcesKey& s) const
            {
                return (std::hash<nvrhi::ITexture*>()(s.texture) << 1)
                    ^ std::hash<nvrhi::TextureSubresourceSet>()(s.subresources);
            }
        };
    };

    nvrhi::FramebufferHandle GetCachedFramebuffer(nvrhi::ITexture* texture, nvrhi::TextureSubresourceSet subresources);
    nvrhi::BindingSetHandle GetCachedBindingSet(nvrhi::ITexture* texture, nvrhi::TextureSubresourceSet subresources);
    nvrhi::GraphicsPipelineHandle GetPipeline(
        std::unordered_map<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle>& cache, nvrhi::IShader* pixelShader,
        nvrhi::IFramebuffer* framebuffer);
    void DrawCubeFaces(nvrhi::ICommandList* commandList, nvrhi::IGraphicsPipeline* pipeline,
        nvrhi::IFramebuffer* framebuffer, nvrhi::IBindingSet* bindingSet, float size);

    nvrhi::DeviceHandle m_Device;
    nvrhi::ShaderHandle m_GeometryShader;               // cubemap_gs
    nvrhi::ShaderHandle m_MipPixelShader;               // mip_ps
    nvrhi::ShaderHandle m_DiffusePixelShader;           // diffuse_probe_ps
    nvrhi::ShaderHandle m_SpecularPixelShader;          // specular_probe_ps
    nvrhi::ShaderHandle m_EnvironmentBrdfPixelShader;   // environment_brdf_ps
    nvrhi::BufferHandle m_LightProbeCB;
    nvrhi::BindingLayoutHandle m_BindingLayout;         // b0 (volatile), s0, t0
    nvrhi::TextureHandle m_IntermediateTexture;
    uint32_t m_IntermediateTextureSize;
    nvrhi::TextureHandle m_EnvironmentBrdfTexture;
    uint32_t m_EnvironmentBrdfTextureSize = 64;
    std::shared_ptr<donut::engine::CommonRenderPasses> m_CommonPasses;

    std::unordered_map<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle> m_BlitPsoCache;
    std::unordered_map<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle> m_DiffusePsoCache;
    std::unordered_map<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle> m_SpecularPsoCache;
    std::unordered_map<TextureSubresourcesKey, nvrhi::FramebufferHandle, TextureSubresourcesKey::Hash> m_FramebufferCache;
    std::unordered_map<TextureSubresourcesKey, nvrhi::BindingSetHandle, TextureSubresourcesKey::Hash> m_BindingSetCache;
};

#pragma once

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <map>
#include <memory>

class SceneLight;

namespace donut::engine
{
    class ShaderFactory;
    class CommonRenderPasses;
    class FramebufferFactory;
    class ICompositeView;
}

namespace fx
{
    // Asteroids.exe: space-dust particles (ctor 0x140067DA0, Render 0x140069250 "Particles",
    // Update 0x14006A130 "Update Particles", Reset 0x140069EA0; object size 0xA8, no vtable)
    //
    // 'numParticles' particles live in a box of 'volumeSize' that is tiled over the world in x/z
    // (y is a single layer at the camera height). Each frame update_cs_main advects them through a
    // noise field; Render draws the box once per tile that intersects the view frustum cut off at
    // 'maxDistance', either with vs_main (3 vertices per particle, 'particlesPerBatch' particles per
    // instance) or with the mesh shader (32 particles per group).
    //
    // FeatureDemo (CreateRenderPasses 0x14002BD60) creates it with numParticles = 250000,
    // volumeSize = (5000, 2000, 5000) and the particle buffer of the previous instance (so the
    // simulation state survives a render-pass rebuild).
    class ParticleSystem
    {
    public:
        ParticleSystem(
            nvrhi::IDevice* device,
            const nvrhi::AutoPtr<donut::engine::ShaderFactory>& shaderFactory,
            const nvrhi::AutoPtr<donut::engine::CommonRenderPasses>& commonPasses,
            const nvrhi::AutoPtr<donut::engine::FramebufferFactory>& framebufferFactory,
            const donut::engine::ICompositeView& compositeView,
            uint32_t numParticles,
            const donut::math::float3& volumeSize,
            nvrhi::IBuffer* existingParticleBuffer);

        // cameraOffset: FeatureDemo camera offset (+640, = -camera world position; the views are
        // camera relative). maxDistance: 5000. particlesPerBatch: UIData+228. useMeshShader: UIData+226.
        void Render(
            nvrhi::ICommandList* commandList,
            const donut::engine::ICompositeView& compositeView,
            const nvrhi::AutoPtr<donut::engine::FramebufferFactory>& framebufferFactory,
            const SceneLight& light,
            const donut::math::float3& cameraOffset,
            float maxDistance,
            uint32_t particlesPerBatch,
            bool useMeshShader);

        // time: FeatureDemo+100 (seconds), deltaTime: FeatureDemo+104 (clamped to [0, 1/30]).
        void Update(nvrhi::ICommandList* commandList, float time, float deltaTime);

        // Re-seeds every particle (std::mt19937 with the default seed) and uploads the buffer.
        void Reset(nvrhi::ICommandList* commandList);

        [[nodiscard]] nvrhi::IBuffer* GetParticleBuffer() const { return m_ParticleBuffer; }

    private:
        nvrhi::BindingSetHandle GetRenderBindingSet(nvrhi::ITexture* shadowMapTexture);

        nvrhi::DeviceHandle m_Device;
        nvrhi::ShaderHandle m_PixelShader;
        nvrhi::ShaderHandle m_VertexShader;
        nvrhi::ShaderHandle m_MeshShader;
        nvrhi::ShaderHandle m_UpdateShader;
        nvrhi::BufferHandle m_ParticleConstants;
        nvrhi::BufferHandle m_ParticleBuffer;
        nvrhi::SamplerHandle m_ShadowSampler;
        nvrhi::BindingLayoutHandle m_RenderBindingLayout;
        nvrhi::BindingSetHandle m_UpdateBindingSet;
        nvrhi::BindingLayoutHandle m_UpdateBindingLayout;
        nvrhi::GraphicsPipelineHandle m_VertexShaderPipeline;
        nvrhi::MeshletPipelineHandle m_MeshShaderPipeline;        // D3D12 mesh-shader mode (SM 6.5)
        nvrhi::GraphicsPipelineHandle m_NvapiMeshShaderPipeline;  // NVAPI mesh-shader mode (wrapped NVAPI PSO)
        nvrhi::BufferHandle m_NvExtensionBuffer;                  // NVAPI mode: dummy g_NvidiaExt (u0)
        bool m_NvapiMeshShader = false;                           // GetMeshShaderMode() == Nvapi at creation
        nvrhi::ComputePipelineHandle m_UpdatePipeline;
        std::map<nvrhi::ITexture*, nvrhi::BindingSetHandle> m_RenderBindingSets;   // keyed by shadow map texture
        nvrhi::AutoPtr<donut::engine::CommonRenderPasses> m_CommonPasses;

        uint32_t m_NumParticles = 0;
        donut::math::float3 m_VolumeSize = 0.f;
        bool m_Initialized = false;   // particle buffer contents valid (reset done or buffer inherited)
    };
}

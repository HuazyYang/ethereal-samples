#include <cstddef>

#include "fx/ParticleSystem.h"

#include "app/RenderHandedness.h"
#include "app/GpuProfiler.h"
#include "fx/FxCommon.h"
#include "scene/Lights.h"
#include "meshlets/MeshShaderMode.h"
#include "meshlets/NvMeshShaderApi.h"

#include <donut/core/log.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/ShadowMap.h>
#include <donut/engine/View.h>
#include <nvrhi/d3d12.h>
#include <nvrhi/utils.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <random>
#include <vector>

using namespace donut::math;
#include <particles_cb.h>

using namespace donut::engine;

namespace fx
{
    // Mesh-shader group size of particles_ms.hlsl (2018: NumParticles >> 5).
    static constexpr uint32_t c_ParticlesPerMeshGroup = 32;

    // 2018 NVAPI particle PSO (0x140067DA0): mesh shader extension with 32 threads, 96 vertices / 32 triangles
    // per group, no task shader; g_NvidiaExt at u0 space0.
    static constexpr uint32_t c_NvParticleExtensionSlot = 0;
    static constexpr uint32_t c_NvParticleMaxVertices = 96;
    static constexpr uint32_t c_NvParticleMaxPrimitives = 32;

    ParticleSystem::ParticleSystem(
        nvrhi::IDevice* device,
        const nvrhi::AutoPtr<ShaderFactory>& shaderFactory,
        const nvrhi::AutoPtr<CommonRenderPasses>& commonPasses,
        const nvrhi::AutoPtr<FramebufferFactory>& framebufferFactory,
        const ICompositeView& compositeView,
        uint32_t numParticles,
        const float3& volumeSize,
        nvrhi::IBuffer* existingParticleBuffer)
        : m_Device(device)
        , m_CommonPasses(commonPasses)
        , m_NumParticles(numParticles)
        , m_VolumeSize(volumeSize)
    {
        m_PixelShader = shaderFactory->CreateShader("demo/particles.hlsl", "ps_main", nullptr, nvrhi::ShaderType::Pixel);
        m_VertexShader = shaderFactory->CreateShader("demo/particles.hlsl", "vs_main", nullptr, nvrhi::ShaderType::Vertex);
        // The 2018 mesh shader was particles.hlsl:ms_main (NVAPI extension, vs_6_0), reconstructed as
        // demo/nvapi/particles.hlsl and used in the NVAPI mesh-shader mode; the D3D12 mode uses the SM 6.5 port
        // particles_ms.hlsl.
        m_NvapiMeshShader = (GetMeshShaderMode() == MeshShaderMode::Nvapi);
        if (m_NvapiMeshShader)
            m_MeshShader = shaderFactory->CreateShader("demo/nvapi/particles.hlsl", "ms_main", nullptr, nvrhi::ShaderType::Vertex);
        else if (device->queryFeatureSupport(nvrhi::Feature::Meshlets))
            m_MeshShader = shaderFactory->CreateShader("demo/particles_ms.hlsl", "ms_main", nullptr, nvrhi::ShaderType::Mesh);
        m_UpdateShader = shaderFactory->CreateShader("demo/particles.hlsl", "update_cs_main", nullptr, nvrhi::ShaderType::Compute);

        device->createBuffer(ConstantBufferDesc(sizeof(ParticleConstants), "ParticleConstants"), &m_ParticleConstants);

        nvrhi::SamplerDesc shadowSamplerDesc;
        shadowSamplerDesc
            .setAllFilters(true)
            .setAllAddressModes(nvrhi::SamplerAddressMode::Border)
            .setBorderColor(nvrhi::Color(1.f))
            .setReductionType(nvrhi::SamplerReductionType::Comparison);
        device->createSampler(shadowSamplerDesc, &m_ShadowSampler);

        if (existingParticleBuffer)
        {
            m_ParticleBuffer = existingParticleBuffer;
            m_Initialized = true;
        }
        else
        {
            nvrhi::BufferDesc bufferDesc;
            bufferDesc.byteSize = uint64_t(sizeof(ParticleInfo)) * numParticles;
            bufferDesc.structStride = sizeof(ParticleInfo);
            bufferDesc.canHaveUAVs = true;
            bufferDesc.debugName = "Particles";
            bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            bufferDesc.keepInitialState = true;
            device->createBuffer(bufferDesc, &m_ParticleBuffer);
            m_Initialized = false;
        }

        // Render bindings (vs_main / ms_main / ps_main).
        // The 2018 layout also had u0 (NVAPI g_NvidiaExt, null resource) for the mesh-shader extension; it is
        // declared in the NVAPI mode only (bound to a dummy buffer), SM 6.5 does not need it.
        nvrhi::BindingLayoutDesc renderLayoutDesc;
        renderLayoutDesc.visibility = nvrhi::ShaderType::All;
        renderLayoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
            nvrhi::BindingLayoutItem::Sampler(0)
        };
        if (m_NvapiMeshShader)
        {
            renderLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(c_NvParticleExtensionSlot));

            nvrhi::BufferDesc extensionDesc;
            extensionDesc.byteSize = 256;       // one NvShaderExtnStruct
            extensionDesc.structStride = 256;
            extensionDesc.canHaveUAVs = true;
            extensionDesc.debugName = "NvShaderExtnUAV";
            extensionDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            extensionDesc.keepInitialState = true;
            device->createBuffer(extensionDesc, &m_NvExtensionBuffer);
        }
        device->createBindingLayout(renderLayoutDesc, &m_RenderBindingLayout);

        const IView* sampleView = compositeView.GetChildView(ViewType::PLANAR, 0);
        const nvrhi::FramebufferInfo framebufferInfo =
            framebufferFactory->GetFramebuffer(*sampleView)->getFramebufferInfo().getInfo();

        nvrhi::RenderState renderState;
        renderState.blendState.targets[0] = BlendStateRT(nvrhi::BlendFactor::SrcAlpha, nvrhi::BlendFactor::InvSrcAlpha);
        renderState.rasterState.setCullNone();
        renderState.depthStencilState
            .enableDepthTest()
            .disableDepthWrite()
            .disableStencil()
            .setDepthFunc(sampleView->IsReverseDepth()
                ? nvrhi::ComparisonFunc::GreaterOrEqual
                : nvrhi::ComparisonFunc::LessOrEqual);

        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
        pipelineDesc.VS = m_VertexShader;
        pipelineDesc.PS = m_PixelShader;
        pipelineDesc.bindingLayouts = { m_RenderBindingLayout };
        pipelineDesc.renderState = renderState;
        device->createGraphicsPipeline1(pipelineDesc, framebufferInfo, &m_VertexShaderPipeline);

        // The 2018 code built this PSO through NVAPI (NvAPI_D3D12_CreateGraphicsPipelineState with the
        // shader extension slot u0 and the mesh-shader extension, 96 vertices / 32 primitives per group); the
        // NVAPI mode does the same, the D3D12 mode creates an SM 6.5 mesh pipeline.
        if (m_MeshShader && m_NvapiMeshShader)
        {
            NvMeshPipelineDesc nvDesc;
            nvDesc.meshShader = m_MeshShader;
            nvDesc.pixelShader = m_PixelShader;
            nvDesc.renderState = renderState;
            nvDesc.bindingLayouts = { m_RenderBindingLayout };
            nvDesc.extensionUavSlot = c_NvParticleExtensionSlot;
            nvDesc.extensionRegisterSpace = 0;
            nvDesc.numThreads = c_ParticlesPerMeshGroup;
            nvDesc.maxVertices = c_NvParticleMaxVertices;
            nvDesc.maxPrimitives = c_NvParticleMaxPrimitives;
            m_NvapiMeshShaderPipeline = CreateNvMeshPipeline(device, nvDesc, framebufferInfo);
        }
        else if (m_MeshShader)
        {
            nvrhi::MeshletPipelineDesc meshletDesc;
            meshletDesc.primType = nvrhi::PrimitiveType::TriangleList;
            meshletDesc.MS = m_MeshShader;
            meshletDesc.PS = m_PixelShader;
            meshletDesc.bindingLayouts = { m_RenderBindingLayout };
            meshletDesc.renderState = renderState;
            device->createMeshletPipeline1(meshletDesc, framebufferInfo, &m_MeshShaderPipeline);
        }
        if (!m_MeshShaderPipeline && !m_NvapiMeshShaderPipeline)
            donut::log::error("Failed to create a meshlet PSO for particles");

        // Simulation bindings (update_cs_main): b0 constants, u0 particles.
        nvrhi::BindingSetDesc updateSetDesc;
        updateSetDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_ParticleConstants),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_ParticleBuffer)
        };
        nvrhi::utils::CreateBindingSetAndLayout(device, nvrhi::ShaderType::Compute, 0, updateSetDesc,
            m_UpdateBindingLayout, m_UpdateBindingSet);

        nvrhi::ComputePipelineDesc computeDesc;
        computeDesc.CS = m_UpdateShader;
        computeDesc.bindingLayouts = { m_UpdateBindingLayout };
        device->createComputePipeline(computeDesc, &m_UpdatePipeline);
    }

    nvrhi::BindingSetHandle ParticleSystem::GetRenderBindingSet(nvrhi::ITexture* shadowMapTexture)
    {
        nvrhi::BindingSetHandle& bindingSet = m_RenderBindingSets[shadowMapTexture];
        if (!bindingSet)
        {
            nvrhi::BindingSetDesc setDesc;
            setDesc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, m_ParticleConstants),
                nvrhi::BindingSetItem::Texture_SRV(0, shadowMapTexture),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_ParticleBuffer),
                nvrhi::BindingSetItem::Sampler(0, m_ShadowSampler)
            };
            if (m_NvExtensionBuffer)
                setDesc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_UAV(c_NvParticleExtensionSlot, m_NvExtensionBuffer));
            m_Device->createBindingSet(setDesc, m_RenderBindingLayout, &bindingSet);
        }
        return bindingSet;
    }

    void ParticleSystem::Render(
        nvrhi::ICommandList* commandList,
        const ICompositeView& compositeView,
        const nvrhi::AutoPtr<FramebufferFactory>& framebufferFactory,
        const SceneLight& light,
        const float3& cameraOffset,
        float maxDistance,
        uint32_t particlesPerBatch,
        bool useMeshShader)
    {
        const uint32_t batchSize = std::min(std::max(particlesPerBatch, 1u), m_NumParticles);

        demo::ProfBegin(commandList, "Particles");

        if (!m_Initialized)
        {
            Reset(commandList);
            m_Initialized = true;
        }

        nvrhi::ITexture* shadowMapTexture = light.shadowMap
            ? light.shadowMap->GetTexture()
            : m_CommonPasses->m_BlackTexture2DArray.Get();
        nvrhi::IBindingSet* bindingSet = GetRenderBindingSet(shadowMapTexture);

        // deviation: fall back to the vertex-shader path when the device has no mesh shaders
        // (the 2018 code selected the pipeline unconditionally).
        const bool meshPath = useMeshShader && m_MeshShaderPipeline;
        const bool nvapiMeshPath = useMeshShader && m_NvapiMeshShaderPipeline;
        if (m_NvExtensionBuffer)
            commandList->setEnableUavBarriersForBuffer(m_NvExtensionBuffer, false);

        for (uint32_t viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
        {
            const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);

            // Tiles of the particle volume touched by the view frustum, cut off at maxDistance.
            frustum viewFrustum = view->GetViewFrustum();
            const plane& nearPlane = viewFrustum.planes[frustum::NEAR_PLANE];
            viewFrustum.planes[frustum::FAR_PLANE] = plane(-nearPlane.normal, maxDistance - nearPlane.distance);

            float2 tileMin = float2(FLT_MAX);
            float2 tileMax = float2(-FLT_MAX);
            for (int corner = 0; corner < int(frustum::numCorners); corner++)
            {
                const float3 p = viewFrustum.getCorner(corner);
                const float2 tile = float2(
                    floorf((p.x - cameraOffset.x) / m_VolumeSize.x),
                    floorf((p.z - cameraOffset.z) / m_VolumeSize.z));
                tileMin = min(tileMin, tile);
                tileMax = max(tileMax, tile);
            }

            ParticleConstants constants{};
            constants.matWorldToClip = view->GetViewProjectionMatrix(false);
            const float4x4 projection = view->GetProjectionMatrix(false);
            // A sprite size: magnitudes, or the sprites would be mirrored.
            constants.screenScale = demo::ProjectionScale(projection);
            constants.maxDistance = maxDistance;
            constants.particlesPerBatch = batchSize;
            FillLightConstants2018(light, constants.light);
            FillShadowConstants2018(light.shadowMap.Get(), constants.light, constants.shadows, PARTICLES_MAX_SHADOWS, false);

            nvrhi::IFramebuffer* framebuffer = framebufferFactory->GetFramebuffer(*view);
            bool stateSet = false;

            for (float tileX = tileMin.x; tileX <= tileMax.x; tileX += 1.f)
            {
                for (float tileZ = tileMin.y; tileZ <= tileMax.y; tileZ += 1.f)
                {
                    constants.positionOffset = float3(
                        tileX * m_VolumeSize.x + cameraOffset.x,
                        cameraOffset.y + m_VolumeSize.y * 0.f,
                        tileZ * m_VolumeSize.z + cameraOffset.z);
                    commandList->writeBuffer(m_ParticleConstants, &constants, sizeof(constants));

                    if (nvapiMeshPath)
                    {
                        // 2018 (0x140069250): setGraphicsState with the wrapped NVAPI PSO, the nvrhi volatile
                        // constant buffer update (0x1401A9320) and NVAPI DispatchMeshTasks(NumParticles >> 5).
                        if (!stateSet)
                        {
                            nvrhi::GraphicsState state;
                            state.pipeline = m_NvapiMeshShaderPipeline;
                            state.framebuffer = framebuffer;
                            state.bindings = { bindingSet };
                            state.viewport = view->GetViewportState();
                            commandList->setGraphicsState(state);
                            stateSet = true;
                        }
                        nvrhi::d3d12::ICommandList* d3d12CommandList = static_cast<nvrhi::d3d12::ICommandList*>(
                            commandList->getNativeObject(nvrhi::ObjectTypes::Nvrhi_D3D12_CommandList));
                        if (d3d12CommandList)
                            d3d12CommandList->updateGraphicsVolatileBuffers();
                        NvDispatchMeshTasks(commandList, m_NumParticles >> 5);
                    }
                    else if (meshPath)
                    {
                        if (!stateSet)
                        {
                            nvrhi::MeshletState state;
                            state.pipeline = m_MeshShaderPipeline;
                            state.framebuffer = framebuffer;
                            state.bindings = { bindingSet };
                            state.viewport = view->GetViewportState();
                            commandList->setMeshletState(state);
                            stateSet = true;
                        }
                        commandList->dispatchMesh(m_NumParticles / c_ParticlesPerMeshGroup);
                    }
                    else
                    {
                        if (!stateSet)
                        {
                            nvrhi::GraphicsState state;
                            state.pipeline = m_VertexShaderPipeline;
                            state.framebuffer = framebuffer;
                            state.bindings = { bindingSet };
                            state.viewport = view->GetViewportState();
                            commandList->setGraphicsState(state);
                            stateSet = true;
                        }

                        nvrhi::DrawArguments args;
                        args.vertexCount = 3 * batchSize;
                        args.instanceCount = m_NumParticles / batchSize;
                        commandList->draw(args);
                    }
                }
            }
        }

        if (m_NvExtensionBuffer)
            commandList->setEnableUavBarriersForBuffer(m_NvExtensionBuffer, true);

        demo::ProfEnd(commandList);
    }

    void ParticleSystem::Update(nvrhi::ICommandList* commandList, float time, float deltaTime)
    {
        demo::ProfBegin(commandList, "Update Particles");

        ParticleConstants constants{};
        constants.time = time;
        constants.deltaTime = std::min(std::max(0.f, deltaTime), 1.f / 30.f);
        constants.volumeSize = m_VolumeSize;
        commandList->writeBuffer(m_ParticleConstants, &constants, sizeof(constants));

        nvrhi::ComputeState state;
        state.pipeline = m_UpdatePipeline;
        state.bindings = { m_UpdateBindingSet };
        commandList->setComputeState(state);
        commandList->dispatch((m_NumParticles + PARTICLES_UPDATE_GROUP_SIZE - 1) / PARTICLES_UPDATE_GROUP_SIZE, 1);

        demo::ProfEnd(commandList);
    }

    void ParticleSystem::Reset(nvrhi::ICommandList* commandList)
    {
        std::mt19937 rng;   // default seed 5489, as in the 2018 code
        std::uniform_real_distribution<float> uniform(0.f, 1.f);

        std::vector<ParticleInfo> particles(m_NumParticles);
        for (ParticleInfo& particle : particles)
        {
            // Draw order as in the binary: z, y, x, brightness.
            const float rz = uniform(rng);
            const float ry = uniform(rng);
            const float rx = uniform(rng);
            particle.position = float3(rx * m_VolumeSize.x, ry * m_VolumeSize.y, rz * m_VolumeSize.z);
            particle.velocity = 0.f;
            particle.brightness = uniform(rng) * 2.f + 0.1f;
            particle.padding = 0.f;
        }

        commandList->writeBuffer(m_ParticleBuffer, particles.data(), particles.size() * sizeof(ParticleInfo));
    }
}

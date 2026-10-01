#pragma once

#include <donut/engine/View.h>
#include <donut/engine/SceneTypes.h>
#include <donut/render/GeometryPasses.h>
#include <donut/render/DrawStrategy.h>
#include <mutex>
#include <unordered_map>
#include "VXGITypes.h"

namespace donut::engine {
class ShaderFactory;
class Light;
class CommonRenderPasses;
class FramebufferFactory;
class MaterialBindingCache;
struct Material;
struct LightProbe;
}  // namespace donut::engine

namespace vxgi {

class VoxelRenderer;
class ViewTracer;

class VoxelizationView : public donut::engine::PlanarView {
public:
   void SetClipRegions(uint32_t numRegions, dm::box3* regions);
   const std::vector<dm::box3>& GetClipRegions() const;

protected:
   std::vector<dm::box3> m_ClipRegions;
};

class VoxelizationInstancedDrawStrategy: public donut::render::IDrawStrategy {
public:
   void PrepareForView(donut::engine::SceneGraphNode* rootNode, const donut::engine::IView& view) override;

   const donut::render::DrawItem* GetNextItem() override;

   [[nodiscard]] size_t GetChunkSize() const { return m_ChunkSize; }
   void SetChunkSize(size_t size) { m_ChunkSize = std::max<size_t>(size, 1u); }

private:
   void FillChunk();
   bool TestClipSceneGraphNode(const dm::box3 &boundingBox);

   std::vector<dm::box3> m_ClipRegions;
   donut::engine::SceneGraphWalker m_Walker;
   std::vector<donut::render::DrawItem> m_InstanceChunk;
   std::vector<const donut::render::DrawItem*> m_InstancePtrChunk;
   size_t m_ReadPtr = 0;
   size_t m_ChunkSize = 128;
};

struct VXGIShadingPassPipelineKey {
    donut::engine::MaterialDomain domain = donut::engine::MaterialDomain::Opaque;
    nvrhi::RasterCullMode cullMode = nvrhi::RasterCullMode::Back;
    bool frontCounterClockwise = false;
    bool reverseDepth = false;
    nvrhi::VariableRateShadingState shadingRateState{};
    bool useForEmittance = true;
    bool useCoverageSuperSampling = false;

    bool operator==(const VXGIShadingPassPipelineKey& other) const {
        return domain == other.domain && cullMode == other.cullMode &&
               frontCounterClockwise == other.frontCounterClockwise &&
               reverseDepth == other.reverseDepth &&
               shadingRateState == other.shadingRateState &&
               useForEmittance == other.useForEmittance &&
               useCoverageSuperSampling == other.useCoverageSuperSampling;
    }

    bool operator!=(const VXGIShadingPassPipelineKey& other) const {
        return !(*this == other);
    }
};

}

namespace std {
template <>
struct hash<std::pair<nvrhi::ITexture*, nvrhi::ITexture*>> {
    size_t operator()(
        const std::pair<nvrhi::ITexture*, nvrhi::ITexture*>& v) const noexcept {
        auto h = hash<nvrhi::ITexture*>();
        return h(v.first) ^ (h(v.second) << 8);
    }
};

template <>
struct hash<vxgi::VXGIShadingPassPipelineKey> {
    std::size_t operator()(
        vxgi::VXGIShadingPassPipelineKey const& key) const noexcept {
        size_t hash = 0;
        nvrhi::hash_combine(hash, key.domain);
        nvrhi::hash_combine(hash, key.cullMode);
        nvrhi::hash_combine(hash, key.frontCounterClockwise);
        nvrhi::hash_combine(hash, key.reverseDepth);
        nvrhi::hash_combine(hash, key.shadingRateState);
        nvrhi::hash_combine(hash, key.useForEmittance);
        nvrhi::hash_combine(hash, key.useCoverageSuperSampling);
        return hash;
    }
};
}  // namespace std

namespace vxgi {

class VoxelShadingPass : public donut::render::IGeometryPass {
 public:
    class Context : public donut::render::GeometryPassContext {
     public:
        nvrhi::ICommandList* commandList;
        nvrhi::BindingSetHandle shadingBindingSet;
        nvrhi::BindingSetHandle inputBindingSet;
        VXGIShadingPassPipelineKey keyTemplate;

        uint32_t positionOffset = 0;
        uint32_t texCoordOffset = 0;
        uint32_t normalOffset = 0;
        uint32_t tangentOffset = 0;

        bool isEmittance = false;
    };

    struct CreateParameters {
        donut::engine::MaterialBindingCache* materialBindings = nullptr;
        bool trackLiveness = true;

        // Switches between loading vertex data through the Input Assembler (true) or buffer
        // SRVs (false). Using Buffer SRVs is often faster.
        bool useInputAssembler = false;

        uint32_t numConstantBufferVersions = 16;
    };

 protected:
    nvrhi::IDevice *m_Device;
    nvrhi::AutoPtr<donut::engine::ShaderFactory> m_ShaderFactory;

    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::ShaderHandle m_VoxelizeVS;

    nvrhi::SamplerHandle m_ShadowSampler;
    nvrhi::BindingLayoutHandle m_ViewBindingLayout;
    nvrhi::BindingSetHandle m_ViewBindingSet;
    nvrhi::BindingLayoutHandle m_ShadingBindingLayout;
    nvrhi::BindingLayoutHandle m_InputBindingLayout;
    nvrhi::BufferHandle m_ForwardViewCB;
    nvrhi::BufferHandle m_ForwardLightCB;
    bool m_TrackLiveness = true;
    bool m_IsDX11 = false;
    bool m_UseInputAssembler = false;
    std::mutex m_Mutex;

    std::unordered_map<VXGIShadingPassPipelineKey, nvrhi::GraphicsPipelineHandle>
        m_Pipelines;
    std::unordered_map<std::pair<nvrhi::ITexture*, nvrhi::ITexture*>,
                       nvrhi::BindingSetHandle>
        m_ShadingBindingSets;
    std::unordered_map<const donut::engine::BufferGroup*, nvrhi::BindingSetHandle> m_InputBindingSets;

    nvrhi::AutoPtr<donut::engine::CommonRenderPasses> m_CommonPasses;
    nvrhi::AutoPtr<donut::engine::MaterialBindingCache> m_MaterialBindings;

    nvrhi::AutoPtr<VoxelRenderer> m_VoxelRenderer;
    nvrhi::AutoPtr<ViewTracer> m_GIViewTracer;
    nvrhi::AutoPtr<ViewTracer> m_AOViewTracer;

    virtual nvrhi::ShaderHandle CreateVertexShader(const CreateParameters& params);
    virtual nvrhi::InputLayoutHandle CreateInputLayout(nvrhi::IShader* vertexShader,
                                                       const CreateParameters& params);
    virtual nvrhi::BindingLayoutHandle CreateViewBindingLayout();
    virtual nvrhi::BindingSetHandle CreateViewBindingSet();
    virtual nvrhi::BindingLayoutHandle CreateShadingBindingLayout();
    virtual nvrhi::BindingSetHandle CreateShadingBindingSet(
        nvrhi::ITexture* shadowMapTexture, nvrhi::ITexture* diffuse,
        nvrhi::ITexture* specular, nvrhi::ITexture* environmentBrdf);
    virtual nvrhi::BindingLayoutHandle CreateInputBindingLayout();
    virtual nvrhi::BindingSetHandle CreateInputBindingSet(
        const donut::engine::BufferGroup* bufferGroup);
    virtual nvrhi::AutoPtr<donut::engine::MaterialBindingCache> CreateMaterialBindingCache(
        donut::engine::CommonRenderPasses& commonPasses);
    virtual nvrhi::GraphicsPipelineHandle CreateGraphicsPipeline(
        VXGIShadingPassPipelineKey const& key,
        nvrhi::FramebufferInfo const& framebufferInfo);
    nvrhi::BindingSetHandle GetOrCreateInputBindingSet(
        const donut::engine::BufferGroup* bufferGroup);

 public:
    VoxelShadingPass(nvrhi::IDevice* device, donut::engine::ShaderFactory *shaderFactory, donut::engine::CommonRenderPasses* commonPasses);
    ~VoxelShadingPass();

    virtual void Init(const CreateParameters& params);

    void ResetBindingCache();

    void SetVoxelizationParameters(const vxgi::VoxelizationParameters &params);
    void PrepareForOpacityVoxelization(nvrhi::ICommandList *commandList, UpdateVoxelizationParameters& params, bool* performOpacityVoxelization,
                                       bool* performEmittanceVoxelization);
    void GetVoxelizationViewMatrix(dm::float4x4& viewMatrix);
    bool GetInvalidatedRegions(uint numMaxRegions, uint* numRegions, box3* regions);
    void PrepareForEmittanceVoxelization();
    void FinalizeVoxelization(nvrhi::ICommandList *commandList);

    void RenderDebug(nvrhi::ICommandList *commandList, const DebugRenderParameters &params);

    void ComputeDiffuseChannel(nvrhi::ICommandList* commandList, const DiffuseTracingParameters& params,
                               nvrhi::ITexture** indirectDiffuse, const ViewTracerInputBuffers* inputBuffers,
                               const ViewTracerInputBuffers* inputBuffersPreviousFrame);

    void ComputeAmbientChannel(nvrhi::ICommandList* commandList, const DiffuseTracingParameters& params,
                                nvrhi::ITexture** ambientTexture, const ViewTracerInputBuffers* inputBuffers,
                                const ViewTracerInputBuffers* inputBuffersPreviousFrame);

    void ComputeSpecularChannel(nvrhi::ICommandList* commandList, const SpecularTracingParameters& params,
                                nvrhi::ITexture** indirectSpecular, const ViewTracerInputBuffers* inputBuffers,
                                const ViewTracerInputBuffers* inputBuffersPreviousFrame);

    virtual void PrepareLights(Context& context, nvrhi::ICommandList* commandList,
                               const std::vector<nvrhi::AutoPtr<donut::engine::Light>>& lights,
                               dm::float3 ambientColorTop, dm::float3 ambientColorBottom,
                               const std::vector<nvrhi::AutoPtr<donut::engine::LightProbe>>& lightProbes);

    // IGeometryPass implementation

    [[nodiscard]] donut::engine::ViewType::Enum GetSupportedViewTypes() const override;
    void SetupView(donut::render::GeometryPassContext& context, nvrhi::ICommandList* commandList,
                   const donut::engine::IView* view, const donut::engine::IView* viewPrev) override;
    bool SetupMaterial(donut::render::GeometryPassContext& context, const donut::engine::Material* material,
                       nvrhi::RasterCullMode cullMode,
                       nvrhi::GraphicsState& state) override;
    void SetupInputBuffers(donut::render::GeometryPassContext& context, const donut::engine::BufferGroup* buffers,
                           nvrhi::GraphicsState& state) override;
    void SetPushConstants(donut::render::GeometryPassContext& context, nvrhi::ICommandList* commandList,
                          nvrhi::GraphicsState& state, nvrhi::DrawArguments& args) override;
};

}  // namespace vxgi
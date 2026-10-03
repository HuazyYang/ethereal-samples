// depth-map (gDepthMap): lucy.obj is rasterized with Simple.hlsl into a colour
// target and an R32_FLOAT post-projection depth target; the depth target is
// shared with CUDA, copied into a gp buffer and given to VolRenderer as the
// depth buffer that terminates the volume rays of explosion.vbx. The volume
// image is then composited over the polygons with FullScreen.hlsl (HAS_TEXTURE_2).
#include <sample-utils/GVDBApp.h>
#include <sample-utils/ObjMesh.h>
#include <gvdb/GVDB.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <nvrhi/utils.h>
#include <imgui.h>

using namespace donut;
using namespace SampleUtils;

namespace {
struct SimpleCommon {
    dm::float4x4 modelMatrix;
    dm::float4x4 viewProjMatrix;
    dm::float4 eyePosW;
    dm::float4 lightPosW;
    dm::float4 ambientColor;
    dm::float4 diffuseFactor;
    dm::float4 specularFactor;
};
}  // namespace

class DepthMapApp : public GVDBApp {
    NVRHI_INHERIT_INTERFACE_TABLE()
 public:
    using GVDBApp::GVDBApp;

    bool OnInit() override {
        // ---- polygon model ----
        log::info("Loading polygon model.");
        m_mesh = loadObjMesh(nullptr, getAssetPath("lucy.obj"), 100.f);
        if (!m_mesh) return false;
        auto meshInstance = MAKE_RC_OBJ_PTR(engine::MeshInstance, m_mesh);
        getSceneGraph()->AttachLeafNode(getSceneGraph()->GetRootNode(), meshInstance)->SetName("lucy");

        nvrhi::CommandListHandle uploadList;
        GetDevice()->createCommandList(nvrhi::CommandListParameters(), &uploadList);
        uploadList->open();
        m_meshBuffers = uploadMeshNVRHI(GetDevice(), uploadList, m_mesh);
        uploadList->close();
        GetDevice()->executeCommandList(uploadList);

        // ---- volume ----
        log::info("Loading volume data.");
        m_volume = createVolume();
        if (!m_volume) return false;
        {
            nvrhi::AutoPtr<nvrhi::IDataBlob> vbx;
            auto fs = MAKE_RC_OBJ_PTR(vfs::NativeFileSystem);
            if (NVRHI_FAILED(fs->readFile(getAssetPath("explosion.vbx"), &vbx)) || !vbx) {
                log::error("Cannot find vbx file.");
                return false;
            }
            nvrhi::AutoPtr<gvdb::IGVDBSerializer> serializer;
            if (NVRHI_FAILED(gvdb::createGVDBSerializer(m_volume, &serializer))) return false;
            if (NVRHI_FAILED(serializer->loadVBX(vbx, nullptr))) {
                log::error("Failed to load the VBX file.");
                return false;
            }
        }
        if (NVRHI_FAILED(gvdb::createGVDBVoxelOps(m_volume, &m_ops))) return false;
        m_ops->updateApron();

        m_instance = addVolumeInstance(m_volume, volumeTransform(), "explosion");
        gvdb::VolumeRenderAttributes &attrs = m_instance->GetRenderAttributes();
        attrs.channel = 0;
        attrs.shading = gvdb::VolumeShading::Volume;
        attrs.steps = {0.5f, 16.f, 0.5f};
        attrs.extinct = {-1.f, 1.f, 0.f};
        attrs.threshold = {0.1f, 0.f, 0.5f};
        attrs.cutoff = {0.005f, 0.005f, 0.f};
        attrs.transferFunction = MAKE_RC_OBJ_PTR(gvdb::VolumeTransferFunction, getGPDevice());
        attrs.transferFunction->setLinear(0.00f, 0.10f, {0, 0, 0, 0}, {0, 0, 0, 0});
        attrs.transferFunction->setLinear(0.10f, 0.50f, {1, 1, 0, 0.1f}, {1, 0, 0, 0.3f});
        attrs.transferFunction->setLinear(0.50f, 0.75f, {1, 0, 0, 0.3f}, {0.2f, 0.2f, 0.2f, 0.1f});
        attrs.transferFunction->setLinear(0.75f, 1.00f, {0.2f, 0.2f, 0.2f, 0.1f}, {0.1f, 0.1f, 0.1f, 0.3f});
        attrs.transferFunction->commit(getGPQueue());

        m_renderer = std::make_unique<VolRenderer>(getGPDevice(), getGPQueue(), getVFS());
        m_renderer->getViewParams().backgroundColor = {0.1f, 0.2f, 0.4f, 1.f};

        getCamera()->zNear = 0.1f;
        getCamera()->zFar = 1000.f;
        getCameraOrbit().setOrbit({-40.f, 20.f, 0.f}, {0.f, 0.f, 0.f}, 400.f);
        getLightOrbit().setOrbit({299.f, 57.3f, 0.f}, {0.f, 0.f, 0.f}, 500.f);

        if (!createPipelines()) return false;
        GetDevice()->createCommandList(nvrhi::CommandListParameters(), &m_rasterList);
        createRasterTargets(getPresenter()->getWidth(0), getPresenter()->getHeight(0));
        return true;
    }

    dm::affine3 volumeTransform() const {
        return makeVolumeTransform({-125.f, -160.f, -125.f}, dm::float3(0.5f), m_rotate, m_translate);
    }

    bool createPipelines() {
        auto *sf = getShaderFactory();
        // Raster pass (Simple.hlsl): colour + post-projection depth.
        {
            auto vs = sf->CreateShader("Simple.hlsl", "VSMain", nullptr, nvrhi::ShaderType::Vertex);
            auto ps = sf->CreateShader("Simple.hlsl", "PSMain", nullptr, nvrhi::ShaderType::Pixel);
            if (!vs || !ps) return false;

            nvrhi::VertexAttributeDesc attributes[] = {
                nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0).setElementStride(sizeof(dm::float3)),
                nvrhi::VertexAttributeDesc().setName("NORMAL").setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(1).setElementStride(sizeof(dm::float3)),
            };
            nvrhi::InputLayoutHandle inputLayout;
            GetDevice()->createInputLayout(attributes, 2, vs, &inputLayout);

            m_constants = nullptr;
            GetDevice()->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(SimpleCommon), "SimpleCommon", 16), &m_constants);

            nvrhi::BindingSetDesc setDesc;
            setDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_constants)};
            if (!nvrhi::utils::CreateBindingSetAndLayout(GetDevice(), nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel, 0,
                                                         setDesc, m_rasterLayout, m_rasterSet))
                return false;

            nvrhi::GraphicsPipelineDesc psoDesc;
            psoDesc.primType = nvrhi::PrimitiveType::TriangleList;
            psoDesc.inputLayout = inputLayout;
            psoDesc.VS = vs;
            psoDesc.PS = ps;
            psoDesc.bindingLayouts = {m_rasterLayout};
            psoDesc.renderState.rasterState.setCullNone();
            psoDesc.renderState.depthStencilState.depthTestEnable = true;
            psoDesc.renderState.depthStencilState.depthWriteEnable = true;
            psoDesc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;
            nvrhi::FramebufferInfo fbInfo;
            fbInfo.colorFormats = {nvrhi::Format::RGBA8_UNORM, nvrhi::Format::R32_FLOAT};
            fbInfo.depthFormat = nvrhi::Format::D24S8;
            GetDevice()->createGraphicsPipeline1(psoDesc, fbInfo, &m_rasterPipeline);
            if (!m_rasterPipeline) return false;
        }
        // Composite pass (FullScreen.hlsl with the second texture).
        {
            auto vs = sf->CreateShader("FullScreen.hlsl", "VSMain", nullptr, nvrhi::ShaderType::Vertex);
            std::vector<engine::ShaderMacro> defines = {{"HAS_TEXTURE_2", "1"}};
            auto ps = sf->CreateShader("FullScreen.hlsl", "PSMain", &defines, nvrhi::ShaderType::Pixel);
            if (!vs || !ps) return false;

            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
            layoutDesc.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(dm::float4)),
                                   nvrhi::BindingLayoutItem::Sampler(0), nvrhi::BindingLayoutItem::Texture_SRV(0),
                                   nvrhi::BindingLayoutItem::Texture_SRV(1)};
            GetDevice()->createBindingLayout(layoutDesc, &m_compositeLayout);

            nvrhi::SamplerDesc samplerDesc;
            samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::ClampToEdge);
            samplerDesc.setAllFilters(false);
            GetDevice()->createSampler(samplerDesc, &m_sampler);

            nvrhi::GraphicsPipelineDesc psoDesc;
            psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
            psoDesc.VS = vs;
            psoDesc.PS = ps;
            psoDesc.bindingLayouts = {m_compositeLayout};
            psoDesc.renderState.depthStencilState.depthTestEnable = false;
            psoDesc.renderState.depthStencilState.depthWriteEnable = false;
            psoDesc.renderState.rasterState.setCullNone();
            GetDevice()->createGraphicsPipeline1(psoDesc, GetDeviceManager()->GetFramebuffer(0)->getFramebufferInfo().getInfo(),
                                                 &m_compositePipeline);
            if (!m_compositePipeline) return false;
        }
        return true;
    }

    void createRasterTargets(uint32_t width, uint32_t height) {
        syncQueue(getGPDevice(), getGPQueue());
        GetDevice()->waitForIdle();
        m_compositeSet = nullptr;
        m_depthGP = nullptr;
        m_depthBufferGP = nullptr;
        m_rasterFramebuffer = nullptr;

        nvrhi::TextureDesc texDesc;
        texDesc.dimension = nvrhi::TextureDimension::Texture2D;
        texDesc.width = width;
        texDesc.height = height;
        texDesc.format = nvrhi::Format::RGBA8_UNORM;
        texDesc.isShaderResource = true;
        texDesc.isRenderTarget = true;
        texDesc.initialState = nvrhi::ResourceStates::RenderTarget;
        texDesc.keepInitialState = true;
        texDesc.debugName = "raster colour";
        GetDevice()->createTexture(texDesc, &m_colorTexture);

        texDesc.format = nvrhi::Format::R32_FLOAT;
        texDesc.debugName = "raster depth (shared)";
        texDesc.sharedResourceFlags = interopSharedFlags(GetDevice());
        GetDevice()->createTexture(texDesc, &m_depthColorTexture);

        texDesc = nvrhi::TextureDesc();
        texDesc.dimension = nvrhi::TextureDimension::Texture2D;
        texDesc.width = width;
        texDesc.height = height;
        texDesc.format = nvrhi::Format::D24S8;
        texDesc.isTypeless = true;   // D3D11 needs a typeless depth resource for the SRV
        texDesc.isRenderTarget = true;
        texDesc.initialState = nvrhi::ResourceStates::DepthWrite;
        texDesc.keepInitialState = true;
        texDesc.debugName = "raster depth-stencil";
        GetDevice()->createTexture(texDesc, &m_depthStencilTexture);

        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(m_colorTexture);
        fbDesc.addColorAttachment(m_depthColorTexture);
        fbDesc.setDepthAttachment(m_depthStencilTexture);
        GetDevice()->createFramebuffer(fbDesc, &m_rasterFramebuffer);

        UT_V_GP(getInteropDevice()->createGPTexture(m_depthColorTexture, &m_depthGP));
        gp::BufferDesc bufDesc;
        bufDesc.byteSize = size_t(width) * height * 4;
        UT_V_GP(getGPDevice()->createBuffer(bufDesc, &m_depthBufferGP));
        m_depthKey = 0;

        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(dm::float4)),
                            nvrhi::BindingSetItem::Sampler(0, m_sampler),
                            nvrhi::BindingSetItem::Texture_SRV(0, m_colorTexture),
                            nvrhi::BindingSetItem::Texture_SRV(1, getPresenter()->getTexture(0))};
        GetDevice()->createBindingSet(setDesc, m_compositeLayout, &m_compositeSet);
    }

    void OnResize(uint32_t width, uint32_t height) override { createRasterTargets(width, height); }

    void OnRender(uint32_t width, uint32_t height) override {
        if (!m_rasterFramebuffer || m_colorTexture->getDesc().width != width || m_colorTexture->getDesc().height != height)
            createRasterTargets(width, height);

        getCamera()->verticalFov = verticalFovFromHorizontal(dm::radians(50.f), float(width) / float(height));
        updateScene();
        RenderView view = getRenderView(int(width), int(height));
        auto *interop = getInteropDevice();

        // ---- raster pass (graphics queue), before the gp work of this frame ----
        {
            m_rasterList->open();
            UT_V_GP(interop->acquireNVRHITextureKeyedMutex(m_depthColorTexture, m_depthKey, ~0u));
            const dm::float4 &bg = m_renderer->getViewParams().backgroundColor;
            m_rasterList->clearTextureFloat(m_colorTexture, nvrhi::AllSubresources, nvrhi::Color(bg.x, bg.y, bg.z, 1.f));
            m_rasterList->clearTextureFloat(m_depthColorTexture, nvrhi::AllSubresources, nvrhi::Color(1.f));
            m_rasterList->clearDepthStencilTexture(m_depthStencilTexture, nvrhi::AllSubresources, true, 1.f, true, 0);

            SimpleCommon constants;
            constants.modelMatrix = dm::float4x4::identity();
            constants.viewProjMatrix = view.viewProj;
            constants.eyePosW = dm::float4(view.eye, 1.f);
            constants.lightPosW = dm::float4(view.lightPos, 1.f);
            constants.ambientColor = {0.1f, 0.1f, 0.1f, 1.f};
            constants.diffuseFactor = {0.5f, 0.5f, 0.5f, 1.f};
            constants.specularFactor = {1.f, 1.f, 1.f, 1.f};
            m_rasterList->writeBuffer(m_constants, &constants, sizeof(constants));

            nvrhi::GraphicsState state;
            state.pipeline = m_rasterPipeline;
            state.framebuffer = m_rasterFramebuffer;
            state.bindings = {m_rasterSet};
            state.vertexBuffers = {nvrhi::VertexBufferBinding{m_meshBuffers.positions, 0, 0},
                                   nvrhi::VertexBufferBinding{m_meshBuffers.normals, 1, 0}};
            state.indexBuffer = nvrhi::IndexBufferBinding{m_meshBuffers.indices, nvrhi::Format::R32_UINT, 0};
            state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(width), float(height)));
            m_rasterList->setGraphicsState(state);
            nvrhi::DrawArguments args;
            args.vertexCount = m_meshBuffers.numIndices;
            m_rasterList->drawIndexed(args);

            UT_V_GP(interop->releaseNVRHITextureKeyedMutex(m_depthColorTexture, m_depthKey + 1));
            m_rasterList->close();
            GetDevice()->executeCommandList(m_rasterList);
            UT_V_GP(interop->commitNVRHIQueueSignal(nvrhi::CommandQueue::Graphics));
        }

        // ---- gp: depth texture -> buffer, volume render with the depth buffer ----
        UT_V_GP(interop->commitGPQueueWait(getGPQueue(), nvrhi::CommandQueue::Graphics));
        {
            gp::GraphicsInteropKeyedMutexWaitParams wait = {m_depthGP.Get(), m_depthKey + 1, ~0u};
            UT_V_GP(interop->acquireGPResourceKeyedMutexes(getGPQueue(), &wait, 1));

            gp::TextureCopyLocation src = {};
            src.type = gp::TextureCopyType::SubresourceIndex;
            src.resource = m_depthGP;
            src.subresourceIndex = 0;
            gp::TextureCopyLocation dst = {};
            dst.type = gp::TextureCopyType::PlacedFootprint;
            dst.resource = m_depthBufferGP;
            dst.placeFootprint.format = gp::Format::R32_FLOAT;
            dst.placeFootprint.width = width;
            dst.placeFootprint.height = height;
            dst.placeFootprint.depth = 1;
            dst.placeFootprint.rowPitch = width * 4;
            UT_V_GP(getGPQueue()->copyTextureRegion(dst, 0, 0, 0, src, nullptr));

            gp::GraphicsInteropKeyedMutexSignalParams signal = {m_depthGP.Get(), m_depthKey + 2};
            UT_V_GP(interop->releaseGPResourceKeyedMutexes(getGPQueue(), &signal, 1));
            m_depthKey += 2;
        }
        m_renderer->getViewParams().rayNormalBias = m_bias;
        m_renderer->setDepthBuffer(m_depthBufferGP);
        m_renderer->render(getSceneGraph(), view, getPresenter()->getRenderBuffer(0));
    }

    void OnDrawOverlay(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer) override {
        // Polygons with the volume composited over them by its opacity.
        const nvrhi::FramebufferInfoEx &fbInfo = framebuffer->getFramebufferInfo();
        nvrhi::GraphicsState state;
        state.pipeline = m_compositePipeline;
        state.framebuffer = framebuffer;
        state.bindings = {m_compositeSet};
        state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(fbInfo.width), float(fbInfo.height)));
        commandList->setGraphicsState(state);
        dm::float4 screenST = {1.f, 1.f, 0.f, 0.f};
        commandList->setPushConstants(&screenST, sizeof(screenST));
        nvrhi::DrawArguments args;
        args.vertexCount = 4;
        commandList->draw(args);
    }

    void OnBuildUI() override {
        ImGui::SliderFloat("Depth bias", &m_bias, 0.f, 4.f);
        bool changed = false;
        changed |= ImGui::DragFloat3("Volume translate", &m_translate.x, 1.f);
        changed |= ImGui::DragFloat3("Volume rotate", &m_rotate.x, 0.5f);
        if (changed) setNodeTransform(m_instance->GetNode(), volumeTransform());
        ImGui::Text("Left: orbit, Middle: pan, Right: distance, Shift: light");
    }

 private:
    nvrhi::AutoPtr<engine::MeshInfo> m_mesh;
    MeshNVRHIBuffers m_meshBuffers;
    nvrhi::AutoPtr<gvdb::IGVDBVolume> m_volume;
    nvrhi::AutoPtr<gvdb::IGVDBVoxelOps> m_ops;
    nvrhi::AutoPtr<gvdb::GVDBVolumeInstance> m_instance;
    std::unique_ptr<VolRenderer> m_renderer;
    dm::float3 m_translate = {0.f, 0.f, -25.f};
    dm::float3 m_rotate = dm::float3::zero();
    float m_bias = 1.f;

    nvrhi::CommandListHandle m_rasterList;
    nvrhi::GraphicsPipelineHandle m_rasterPipeline;
    nvrhi::BindingLayoutHandle m_rasterLayout;
    nvrhi::BindingSetHandle m_rasterSet;
    nvrhi::BufferHandle m_constants;
    nvrhi::TextureHandle m_colorTexture;
    nvrhi::TextureHandle m_depthColorTexture;
    nvrhi::TextureHandle m_depthStencilTexture;
    nvrhi::FramebufferHandle m_rasterFramebuffer;
    nvrhi::AutoPtr<gp::ITexture> m_depthGP;
    nvrhi::AutoPtr<gp::IBuffer> m_depthBufferGP;
    uint32_t m_depthKey = 0;

    nvrhi::GraphicsPipelineHandle m_compositePipeline;
    nvrhi::BindingLayoutHandle m_compositeLayout;
    nvrhi::BindingSetHandle m_compositeSet;
    nvrhi::SamplerHandle m_sampler;
};

int main(int argc, const char **argv) {
    GVDBAppOptions options;
    options.title = "GVDB Voxels - depth-map";
    options.sampleName = GVDB_SAMPLE_NAME;
    return runGVDBApp<DepthMapApp>(argc, argv, options);
}

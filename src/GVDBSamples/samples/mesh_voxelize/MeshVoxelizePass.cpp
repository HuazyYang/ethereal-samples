#include <donut/app/DeviceManager.h>
#include <donut/app/ApplicationBase.h>
#include <donut/core/object/AutoPtr.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/BindingCache.h>
#include <donut/app/ImGuiRenderPass.h>
#include <nvrhi/utils.h>
#include <gvdb/GPDeviceCUDA.h>
#include <gvdb/GPDeviceNVRHI.h>
#include <gvdb/GVDB.h>
#include <sample_utils/SampleTypes.h>
#include <sample_utils/Camera.h>
#include <sample_utils/ObjLoader.h>
#include <sample_utils/Voxelizer.h>
#include <sample_utils/VolRenderer.h>
#include <sample_utils/Scene.h>
// NOTE(migration): countof() came from the old ethereal fork's <ethereal/core/math/basics.h>;
// donut has no equivalent, so std::size() is used instead.
#include <iterator>

struct FullscreenCommon {
    dm::float4 screenST;
};

struct TopologyInstanceData {
    dm::float4x4 matLocalToWorld;
    dm::float4 color;
};

class VoxelizePass : public donut::app::IRenderPass {
    friend class VoxelizeGUIPass;

 public:
    VoxelizePass(donut::app::DeviceManager *pDeviceManager) : IRenderPass(pDeviceManager) {}

    bool Init() {
        m_vfs = donut::TakeOver(MAKE_RC_OBJ(donut::vfs::RootFileSystem));
        auto binaryPath = donut::app::GetDirectoryWithExecutable();
        m_vfs->mount("app/asset", binaryPath / "asset");
        m_vfs->mount("app/shaders",
                     binaryPath / "compiled_shaders" /
                         donut::app::GetShaderTypeName(GetDeviceManager()->GetGraphicsAPI()));
        m_vfs->mount("gvdb", binaryPath);
        m_vfs->mount("sample_utils/kernels", binaryPath);

        m_shaderFactory =
            donut::TakeOver(MAKE_RC_OBJ(donut::engine::ShaderFactory, GetDevice(), m_vfs, ""));

        {
            donut::gp::CUDADeviceDesc cudaDeviceDesc;
            cudaDeviceDesc.messageCallback = MAKE_RC_OBJ(SampleUtils::GPDeviceMessageCallback);
            UT_V_GP(donut::gp::createCUDADevice(cudaDeviceDesc, &m_gpDevice));
            cudaDeviceDesc.messageCallback->Release();

            UT_V_GP(donut::createGPAndNVRHIDevice(GetDevice(), m_gpDevice,
                                                       &m_interopDevice));

            donut::gp::DeviceQueueDesc queueDesc;
            queueDesc.priority = donut::gp::DeviceQueuePriority::Normal;
            UT_V_GP(m_gpDevice->createDeviceQueue(queueDesc, &m_gpQueue));

            UT_V_GP(SampleUtils::createVoxelizer(m_gpDevice, m_vfs, &m_voxelizer));
            // TODO(migration): dropped - ETHEREAL_NEW exists in the old ethereal fork but not in donut
            m_volumeRenderer.reset(new SampleUtils::VolRenderer{m_gpDevice, m_vfs});
        }

        m_scene = TakeOver(MAKE_RC_OBJ(gvdb::GVDBScene));
        auto model = TakeOver(MAKE_RC_OBJ(gvdb::GVDBModel));
        model->loadObj(m_vfs, "app/asset/lucy.obj");
        m_scene->addModel(model);
        m_voxelizer->commitGeometry(model);

        m_pivot = {0.3f, 0.45f, 0.3f};
        m_partSize = 100.f;
        m_voxelSize = 0.5f;

        m_voxelizer->configLevels(3, 3, 3, 3, 5);
        m_voxelizer->configAtlas({16u, 16u, 1u}, 1);

        Revoxelize();

        m_volumeRenderer->setSteps(0.5f, 16.f, 0.5f);
        m_volumeRenderer->setVolumeRange(0.25f, 0.f, 1.f);
        m_volumeRenderer->setExtinct(-1.f, 1.1f, 0.f);
        m_volumeRenderer->setCutoff(0.005f, 0.005f, 0.f);
        m_volumeRenderer->setShadowParams(0.f, 0.f, 0.f);
        m_volumeRenderer->setLinearTransferFunc(0.f, 0.5f, {0.f, 0.f, 0.f, 0.f},
                                                {1.f, 1.f, 1.f, 0.5f});
        m_volumeRenderer->setLinearTransferFunc(0.5f, 1.f, {1.f, 1.f, 1.f, 0.5f},
                                                {1.f, 1.f, 1.f, 0.8f});
        m_volumeRenderer->commitTransferFunc();
        m_volumeRenderer->setBackgroundColor({0.1f, 0.2f, 0.4f, 1.f});

        auto camera = TakeOver(MAKE_RC_OBJ(gvdb::GVDBCamera));
        camera->setProjectionRH(true);
        camera->setViewParamsSpherical(m_pivot * m_partSize, 150.f, dm::radians(60.f),
                                       dm::radians(-45.f));
        camera->setProjectParams(dm::radians(50.f), 0.1f, 5000.f);
        m_scene->setCamera(camera);

        auto mainLight = TakeOver(MAKE_RC_OBJ(gvdb::GVDBLight));
        mainLight->setProjectionRH(true);
        mainLight->setViewParamsSpherical(m_pivot * m_partSize * dm::float3{1.3f, 1.f, 1.f},
                                          200.f, dm::radians(32.7f), dm::radians(299.f));
        m_scene->addLight(mainLight);

        CreateRenderBuffers();

        // Create full screen blit PSO
        {
            auto vs = m_shaderFactory->CreateShader("app/shaders/FullScreen.hlsl", "VSMain",
                                                    {}, nvrhi::ShaderType::Vertex);
            auto ps = m_shaderFactory->CreateShader("app/shaders/FullScreen.hlsl", "PSMain",
                                                    {{"HAS_TEXTURE_2", "0"}},
                                                    nvrhi::ShaderType::Pixel);

            nvrhi::BindingLayoutDesc bindingLayoutDesc;
            bindingLayoutDesc.visibility =
                nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
            bindingLayoutDesc.bindings = {
                nvrhi::BindingLayoutItem::PushConstants(0, sizeof(FullscreenCommon)),
                nvrhi::BindingLayoutItem::Sampler(0),
                nvrhi::BindingLayoutItem::Texture_SRV(0)};
            m_fullscreenBindingLayout = GetDevice()->createBindingLayout(bindingLayoutDesc);

            nvrhi::GraphicsPipelineDesc psoDesc;
            psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
            psoDesc.VS = vs;
            psoDesc.PS = ps;
            psoDesc.renderState.rasterState.scissorEnable = true;
            psoDesc.renderState.depthStencilState.depthTestEnable = false;
            psoDesc.renderState.depthStencilState.depthWriteEnable = false;
            psoDesc.bindingLayouts = {m_fullscreenBindingLayout};
            psoDesc.fbInfo = GetDeviceManager()->GetFramebuffer(0)->getFramebufferInfo();

            m_fullscreenPSO = GetDevice()->createGraphicsPipeline(psoDesc);

            nvrhi::SamplerDesc samplerDesc;
            m_fullscreenSampler = GetDevice()->createSampler(samplerDesc);
        }

        m_commandList = GetDevice()->createCommandList();

        m_commandList->open();

        CreateDrawTopologyResources();

        m_commandList->close();
        GetDevice()->executeCommandList(m_commandList);

        return true;
    }

    void Revoxelize() {
        m_voxelizer->getGVDB()->destroyChannels();

        gvdb::GVDBAtlasResourceDesc atlasResDesc = {};
        atlasResDesc.samplerDesc.minFilter = true;
        atlasResDesc.samplerDesc.magFilter = true;
        atlasResDesc.format = gvdb::AtlasFormat::ATLAS_FORMAT_R32_FLOAT;
        atlasResDesc.voidValue = dm::float4::zero();
        m_voxelizer->getGVDB()->addChannel(atlasResDesc);

        dm::affine3 matModelToVolume =
            dm::rotation(dm::float3{0.f, 1.f, 0.f}, dm::radians(-10.f)) *
            dm::translation(m_pivot) * dm::scaling(dm::float3{1.f / m_voxelSize}) *
            dm::scaling(dm::float3{m_partSize});

        m_voxelizer->solidVoxelize(0, m_scene->getModel(0), matModelToVolume, 1.f, 0.5f,
                                   0.f);
        m_volumeRenderer->setTransform(dm::float3{0.f}, dm::float3{m_voxelSize},
                                       dm::float3{0.f}, dm::float3{0.f});
    }

    void Animate(float fElapsedTimeSeconds) override {
        m_scene->getCamera()->update();
        m_scene->getLight(0)->update();
    }

    void Render(nvrhi::IFramebuffer *fb) override {
        const auto &fbInfo = fb->getFramebufferInfo();

        m_interopDevice->commitGPQueueWait(m_gpQueue, nvrhi::CommandQueue::Graphics);

        int w, h;
        GetDeviceManager()->GetWindowDimensions(w, h);

        // Render voxels
        {
            float yslice = 100.f * (1.f - float(m_currMousePos.y) / h);
            m_volumeRenderer->setCrossSection({0.f, yslice, 0.f}, {0.f, 1.f, 0.f});
            m_volumeRenderer->setShading(m_shadeType);
            m_volumeRenderer->render(m_voxelizer->getGVDB(), 0, m_renderBufferGP,
                                     m_scene->getCamera(), fbInfo.width, fbInfo.height,
                                     m_scene->getLight(0));
        }

        // Render cross section
        {
            //  Set crsos section
            {
                dm::float3 world;
                world = m_pivot * (m_partSize / m_voxelSize);
                world.y *= 1.f - float(m_currMousePos.y) / h;
                m_volumeRenderer->setCrossSection(world, dm::float3{world.x, 1.f, world.z});
            }
            m_volumeRenderer->setShading(gvdb::GVDBShadeType::SECTION2D);
            m_volumeRenderer->render(
                m_voxelizer->getGVDB(), 0, m_renderSectionBufferGP, m_scene->getCamera(),
                m_sectionTextureGP->getDesc()->width, m_sectionTextureGP->getDesc()->height,
                m_scene->getLight(0));
        }

        // copy render buffer to screen texture
        {
            donut::gp::GraphicsInteropKeyedMutexWaitParams gpResourceWaitParamsSet[] = {
                {m_screenTextureGP, m_screenTextureKeyMutexIndex++, ~0u},
                {m_sectionTextureGP, m_sectionTextureKeyMutexIndex++, ~0u}};
            GVDB_V_GP(m_interopDevice->acquireGPResourceKeyedMutexes(
                m_gpQueue, gpResourceWaitParamsSet, std::size(gpResourceWaitParamsSet)));

            donut::gp::TextureCopyLocation srcBuffer;
            srcBuffer.resource = m_renderBufferGP;
            srcBuffer.type = donut::gp::TextureCopyType::PlacedFootprint;
            srcBuffer.placeFootprint.width = m_screenTextureGP->getDesc()->width;
            srcBuffer.placeFootprint.height = m_screenTextureGP->getDesc()->height;
            srcBuffer.placeFootprint.depth = m_screenTextureGP->getDesc()->depthOrArraySize;
            srcBuffer.placeFootprint.rowPitch = m_screenTextureGP->getDesc()->width * 4;
            srcBuffer.placeFootprint.format = m_screenTextureGP->getDesc()->format;
            srcBuffer.placeFootprint.offset = 0;
            donut::gp::TextureCopyLocation dstTexture;
            dstTexture.resource = m_screenTextureGP;
            dstTexture.type = donut::gp::TextureCopyType::SubresourceIndex;
            dstTexture.subresourceIndex = 0;
            m_gpQueue->copyTextureRegion(dstTexture, 0, 0, 0, srcBuffer, nullptr);

            srcBuffer.resource = m_renderSectionBufferGP;
            srcBuffer.type = donut::gp::TextureCopyType::PlacedFootprint;
            srcBuffer.placeFootprint.width = m_sectionTextureGP->getDesc()->width;
            srcBuffer.placeFootprint.height = m_sectionTextureGP->getDesc()->height;
            srcBuffer.placeFootprint.depth = 1;
            srcBuffer.placeFootprint.rowPitch = m_sectionTextureGP->getDesc()->width * 4;
            srcBuffer.placeFootprint.format = m_screenTextureGP->getDesc()->format;
            srcBuffer.placeFootprint.offset = 0;
            dstTexture.type = donut::gp::TextureCopyType::SubresourceIndex;
            dstTexture.resource = m_sectionTextureGP;
            dstTexture.subresourceIndex = 0;
            m_gpQueue->copyTextureRegion(dstTexture, 0, 0, 0, srcBuffer, nullptr);

            donut::gp::GraphicsInteropKeyedMutexSignalParams gpResourcSignalParamsSet[] =
                {{m_screenTextureGP, m_screenTextureKeyMutexIndex},
                 {m_sectionTextureGP, m_sectionTextureKeyMutexIndex}};
            GVDB_V_GP(m_interopDevice->releaseGPResourceKeyedMutexes(
                m_gpQueue, gpResourcSignalParamsSet, std::size(gpResourcSignalParamsSet)));
        }

        m_interopDevice->commitGPQueueSignal(m_gpQueue, nvrhi::CommandQueue::Graphics);

        // record graphics render command queue
        m_interopDevice->commitNVRHIQueueWait(nvrhi::CommandQueue::Graphics);

        m_commandList->open();

        m_interopDevice->acquireNVRHITextureKeyedMutex(m_screenTexture,
                                                       m_screenTextureKeyMutexIndex++, ~0u);
        m_interopDevice->acquireNVRHITextureKeyedMutex(
            m_sectionTexture, m_sectionTextureKeyMutexIndex++, ~0u);

        m_commandList->clearTextureFloat(fb->getDesc().colorAttachments[0].texture,
                                         nvrhi::AllSubresources, nvrhi::Color{0.f});

        // fullscreen blit
        {
            nvrhi::GraphicsState state;
            state.framebuffer = fb;
            state.pipeline = m_fullscreenPSO;
            state.bindings = {m_fullscreenBinding};
            state.viewport.addViewport(
                nvrhi::Viewport{float(fbInfo.width), float(fbInfo.height)});
            state.viewport.addScissorRect(
                nvrhi::Rect{(int)fbInfo.width, (int)fbInfo.height});
            m_commandList->setGraphicsState(state);

            nvrhi::DrawArguments drawArgs;
            drawArgs.vertexCount = 4;
            FullscreenCommon fullscreenConst;
            fullscreenConst.screenST = {1.f, 1.f, 0.f, 0.f};
            m_commandList->setPushConstants(&fullscreenConst, sizeof(fullscreenConst));
            m_commandList->draw(drawArgs);

            state.bindings = {m_sectionBinding};
            nvrhi::Rect sectionScreenArea{0, int(fbInfo.width * 0.25f + 0.5f),
                                          int(fbInfo.height * 0.75f), int(fbInfo.height)};
            // state.viewport.viewports = {nvrhi::Viewport{
            //     float(sectionScreenArea.minX), float(sectionScreenArea.maxX),
            //     float(sectionScreenArea.minY), float(sectionScreenArea.maxY), 0.f, 1.f}};
            state.viewport.scissorRects = {sectionScreenArea};
            m_commandList->setGraphicsState(state);

            fullscreenConst.screenST = {4.f, 4.f, 0.f, -3.f};
            m_commandList->setPushConstants(&fullscreenConst, sizeof(fullscreenConst));
            m_commandList->draw(drawArgs);
        }

        if (m_showTopo) DrawTopology(fb);

        m_interopDevice->releaseNVRHITextureKeyedMutex(m_screenTexture,
                                                       m_screenTextureKeyMutexIndex);
        m_interopDevice->releaseNVRHITextureKeyedMutex(m_sectionTexture,
                                                       m_sectionTextureKeyMutexIndex);

        m_commandList->close();

        GetDevice()->executeCommandList(m_commandList);

        m_interopDevice->commitNVRHIQueueSignal(nvrhi::CommandQueue::Graphics);
    }

    void BackBufferResized(const uint32_t width, const uint32_t height,
                           const uint32_t sampleCount) override {
        m_scene->getCamera()->setWindowParams(0, 0, width, height);
        m_scene->getLight(0)->setWindowParams(0, 0, width, height);

        CreateRenderBuffers();

        // recreate fullscreen binding set
        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::PushConstants(0, sizeof(dm::float4)),
            nvrhi::BindingSetItem::Sampler(0, m_fullscreenSampler),
            nvrhi::BindingSetItem::Texture_SRV(0, m_screenTexture)};
        m_fullscreenBinding =
            GetDevice()->createBindingSet(bindingSetDesc, m_fullscreenBindingLayout);

        bindingSetDesc.bindings[2] =
            nvrhi::BindingSetItem::Texture_SRV(0, m_sectionTexture);
        m_sectionBinding =
            GetDevice()->createBindingSet(bindingSetDesc, m_fullscreenBindingLayout);
    }

    bool MouseButtonUpdate(int button, int action, int mods) override {
        auto camera = (mods & GLFW_MOD_SHIFT) ? m_scene->getLight(0) : m_scene->getCamera();

        double xpos, ypos;
        glfwGetCursorPos(GetDeviceManager()->GetWindow(), &xpos, &ypos);

        if (action == GLFW_PRESS) {
            switch (button) {
                case GLFW_MOUSE_BUTTON_LEFT:
                    camera->rotationStart(xpos, ypos);
                    break;
                case GLFW_MOUSE_BUTTON_MIDDLE:
                    camera->translationStart(xpos, ypos);
                    break;
                case GLFW_MOUSE_BUTTON_RIGHT:
                    camera->zoomStart(xpos, ypos);
                    break;
            }
        } else {
            switch (button) {
                case GLFW_MOUSE_BUTTON_LEFT:
                    camera->rotationEnd(xpos, ypos);
                    break;
                case GLFW_MOUSE_BUTTON_MIDDLE:
                    camera->translationEnd(xpos, ypos);
                    break;
                case GLFW_MOUSE_BUTTON_RIGHT:
                    camera->zoomEnd(xpos, ypos);
                    break;
            }
        }

        return false;
    }

    bool MousePosUpdate(double xpos, double ypos) override {
        auto window = GetDeviceManager()->GetWindow();
        bool shift = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                     glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        auto camera = shift ? m_scene->getLight(0) : m_scene->getCamera();

        camera->rotationMove(xpos, ypos);
        camera->translationMove(xpos, ypos);
        camera->zoomMove(xpos, ypos);

        m_currMousePos = {int(xpos), int(ypos)};
        return false;
    }

 private:
    void CreateRenderBuffers() {
        int winWidth, winHeight;
        GetDeviceManager()->GetWindowDimensions(winWidth, winHeight);

        nvrhi::TextureDesc texDesc;
        texDesc.dimension = nvrhi::TextureDimension::Texture2D;
        texDesc.width = winWidth;
        texDesc.height = winHeight;
        texDesc.format = nvrhi::Format::RGBA8_UNORM;
        texDesc.isShaderResource = true;
        texDesc.sharedResourceFlags = nvrhi::SharedResourceFlags::Shared |
                                      nvrhi::SharedResourceFlags::Shared_NTHandle;
        texDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        texDesc.keepInitialState = true;

        m_screenTexture = GetDevice()->createTexture(texDesc);
        m_screenTextureKeyMutexIndex = 0;
        GVDB_V_GP(m_interopDevice->createGPTexture(m_screenTexture, &m_screenTextureGP));

        donut::gp::BufferDesc bufDesc = {};
        bufDesc.byteSize = texDesc.width * texDesc.height * 4;
        GVDB_V_GP(m_gpDevice->createBuffer(bufDesc, &m_renderBufferGP));

        texDesc.width = 256;
        texDesc.height = 256;
        if (!m_sectionTexture) {
            m_sectionTexture = GetDevice()->createTexture(texDesc);
            GVDB_V_GP(
                m_interopDevice->createGPTexture(m_sectionTexture, &m_sectionTextureGP));

            bufDesc.byteSize = texDesc.width * texDesc.height * 4;
            GVDB_V_GP(m_gpDevice->createBuffer(bufDesc, &m_renderSectionBufferGP));

            m_sectionTextureKeyMutexIndex = 0;
        }
    }

    void CreateDrawTopologyResources() {
        dm::float4 positions[] = {{1.f, 1.f, -1.f, 1.f},   {-1.f, 1.f, -1.f, 1.f},
                                  {-1.f, -1.f, -1.f, 1.f}, {1.f, -1.f, -1.f, 1.f},
                                  {1.f, 1.f, 1.f, 1.f},    {-1.f, 1.f, 1.f, 1.f},
                                  {-1.f, -1.f, 1.f, 1.f},  {1.f, -1.f, 1.f, 1.f}};
        uint16_t indices[24] = {0, 1, 1, 2, 2, 3, 3, 0, 0, 4, 1, 5,
                                2, 6, 3, 7, 4, 5, 5, 6, 6, 7, 7, 4};

        nvrhi::BufferDesc bufDesc;
        bufDesc.initialState = nvrhi::ResourceStates::CopyDest;

        bufDesc.isVertexBuffer = true;
        bufDesc.byteSize = sizeof(positions);
        m_box3DVertexBuffer = GetDevice()->createBuffer(bufDesc);

        bufDesc.isVertexBuffer = false;
        bufDesc.isIndexBuffer = true;
        bufDesc.byteSize = sizeof(indices);
        m_box3DIndexBuffer = GetDevice()->createBuffer(bufDesc);

        bufDesc = {};
        bufDesc.isConstantBuffer = true;
        bufDesc.isVolatile = true;
        bufDesc.maxVersions = GetDeviceManager()->GetBackBufferCount();
        bufDesc.byteSize = sizeof(dm::float4x4);
        m_box3DConstBuffer = GetDevice()->createBuffer(bufDesc);

        m_commandList->beginTrackingBufferState(m_box3DVertexBuffer,
                                                nvrhi::ResourceStates::CopyDest);
        m_commandList->beginTrackingBufferState(m_box3DIndexBuffer,
                                                nvrhi::ResourceStates::CopyDest);
        m_commandList->writeBuffer(m_box3DVertexBuffer, positions, sizeof(positions));
        m_commandList->writeBuffer(m_box3DIndexBuffer, indices, sizeof(indices));

        m_commandList->setPermanentBufferState(m_box3DVertexBuffer,
                                               nvrhi::ResourceStates::VertexBuffer);
        m_commandList->setPermanentBufferState(m_box3DIndexBuffer,
                                               nvrhi::ResourceStates::IndexBuffer);
        m_commandList->commitBarriers();

        m_topologyInstanceData.resize(256);
        bufDesc = {};
        bufDesc.structStride = sizeof(TopologyInstanceData);
        bufDesc.byteSize = sizeof(TopologyInstanceData) * m_topologyInstanceData.size();
        bufDesc.keepInitialState = true;
        bufDesc.initialState = nvrhi::ResourceStates::CopyDest;
        m_topologyInstanceBuffer = GetDevice()->createBuffer(bufDesc);

        // pipeline
        {
            auto vs = m_shaderFactory->CreateShader("app/shaders/Box3D.hlsl", "VSMain", {},
                                                    nvrhi::ShaderType::Vertex);
            auto ps = m_shaderFactory->CreateShader("app/shaders/Box3D.hlsl", "PSMain", {},
                                                    nvrhi::ShaderType::Pixel);

            nvrhi::BindingLayoutDesc bindingDesc;
            bindingDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
            bindingDesc.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
                                    nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0)};
            m_box3DBindingLayout = GetDevice()->createBindingLayout(bindingDesc);

            nvrhi::VertexAttributeDesc vertDesc[] = {{"POSITION",
                                                      nvrhi::Format::RGBA32_FLOAT, 1, 0, 0,
                                                      sizeof(dm::float4), false}};
            auto inputLayout = GetDevice()->createInputLayout(vertDesc, 1, vs);

            nvrhi::GraphicsPipelineDesc psoDesc;
            psoDesc.primType = nvrhi::PrimitiveType::LineList;
            psoDesc.inputLayout = inputLayout;
            psoDesc.bindingLayouts = {m_box3DBindingLayout};
            psoDesc.VS = vs;
            psoDesc.PS = ps;
            psoDesc.renderState.depthStencilState.depthTestEnable = false;
            psoDesc.renderState.depthStencilState.depthWriteEnable = false;
            psoDesc.fbInfo = GetDeviceManager()->GetFramebuffer(0)->getFramebufferInfo();

            m_box3DPipeline = GetDevice()->createGraphicsPipeline(psoDesc);
        }

        // Binding set
        {
            nvrhi::BindingSetDesc bindingSetDesc;
            bindingSetDesc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, m_box3DConstBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_topologyInstanceBuffer)};
            m_box3DBindingSet =
                GetDevice()->createBindingSet(bindingSetDesc, m_box3DBindingLayout);
        }
    }

    void DrawTopology(nvrhi::IFramebuffer *fb) {
        auto gvdb = m_voxelizer->getGVDB();

        dm::float4x4 matWorldViewProj = m_volumeRenderer->getTransform() *
                                        m_scene->getCamera()->getViewMatrix() *
                                        m_scene->getCamera()->getProjMatrix();
        m_commandList->writeBuffer(m_box3DConstBuffer, &matWorldViewProj,
                                   sizeof(matWorldViewProj));

        uint32_t fbWidth = fb->getFramebufferInfo().width;
        uint32_t fbHeight = fb->getFramebufferInfo().height;
        nvrhi::GraphicsState state;
        state.pipeline = m_box3DPipeline;
        state.bindings = {m_box3DBindingSet};
        state.vertexBuffers = {{m_box3DVertexBuffer}};
        state.indexBuffer = {m_box3DIndexBuffer, nvrhi::Format::R16_UINT, 0};
        state.framebuffer = fb;
        state.viewport.viewports = {nvrhi::Viewport{float(fbWidth), float(fbHeight)}};
        state.viewport.scissorRects = {nvrhi::Rect{int(fbWidth), int(fbHeight)}};
        m_commandList->setGraphicsState(state);

        nvrhi::DrawArguments drawArgs;

        uint32_t instanceIndex = 0;
        for (uint8_t lev = 0; lev < gvdb->getNumLevels(); ++lev) {
            uint64_t node_cnt = gvdb->getNumUsedNodes(lev);
            dm::float4 color = {gvdb->getColorDim(lev), 1.f};

            for (uint64_t n = 0; n < node_cnt; ++n) {
                auto node = gvdb->getNode(lev, n);
                auto bound = gvdb->getNodeWorldBounds(node);
                auto r = bound.diagonal() * 0.5f;
                auto c = bound.center();

                TopologyInstanceData &instanceData = m_topologyInstanceData[instanceIndex];
                instanceData.matLocalToWorld =
                    dm::float4x4{r.x, 0.f, 0.f, 0.f, 0.f, r.y, 0.f, 0.f,
                                 0.f, 0.f, r.z, 0.f, c.x, c.y, c.z, 1.f};
                instanceData.color = color;
                instanceIndex += 1;

                if (instanceIndex >= m_topologyInstanceData.size()) {
                    m_commandList->writeBuffer(
                        m_topologyInstanceBuffer, m_topologyInstanceData.data(),
                        instanceIndex * sizeof(TopologyInstanceData));

                    drawArgs.vertexCount = 24;
                    drawArgs.instanceCount = instanceIndex;
                    m_commandList->drawIndexed(drawArgs);

                    instanceIndex = 0;
                }
            }
        }

        if (instanceIndex) {
            m_commandList->writeBuffer(m_topologyInstanceBuffer,
                                       m_topologyInstanceData.data(),
                                       instanceIndex * sizeof(TopologyInstanceData));
            drawArgs.vertexCount = 24;
            drawArgs.instanceCount = instanceIndex;
            m_commandList->drawIndexed(drawArgs);
        }
    }

    donut::AutoPtr<donut::vfs::RootFileSystem> m_vfs;
    donut::AutoPtr<donut::gp::IDevice> m_gpDevice;
    donut::AutoPtr<donut::gp::IDeviceQueue> m_gpQueue;

    donut::AutoPtr<SampleUtils::Scene> m_scene;
    donut::AutoPtr<gvdb::GVDB> m_gvdb;
    donut::AutoPtr<SampleUtils::IVoxelizer> m_voxelizer;
    std::unique_ptr<SampleUtils::VolRenderer> m_volumeRenderer;

    nvrhi::TextureHandle m_screenTexture;
    nvrhi::TextureHandle m_sectionTexture;
    donut::AutoPtr<donut::gp::ITexture> m_screenTextureGP;
    donut::AutoPtr<donut::gp::ITexture> m_sectionTextureGP;
    uint32_t m_screenTextureKeyMutexIndex;
    donut::AutoPtr<donut::gp::IBuffer> m_renderBufferGP;
    donut::AutoPtr<donut::gp::IBuffer> m_renderSectionBufferGP;
    uint32_t m_sectionTextureKeyMutexIndex;

    nvrhi::CommandListHandle m_commandList;
    donut::AutoPtr<donut::engine::ShaderFactory> m_shaderFactory;
    nvrhi::GraphicsPipelineHandle m_fullscreenPSO;
    nvrhi::BindingLayoutHandle m_fullscreenBindingLayout;
    nvrhi::BindingSetHandle m_fullscreenBinding;
    nvrhi::BindingSetHandle m_sectionBinding;
    nvrhi::SamplerHandle m_fullscreenSampler;
    donut::AutoPtr<donut::IGPAndNVRHIInteropDevice> m_interopDevice;

    std::vector<TopologyInstanceData> m_topologyInstanceData;
    nvrhi::BufferHandle m_topologyInstanceBuffer;
    nvrhi::BufferHandle m_box3DVertexBuffer;
    nvrhi::BufferHandle m_box3DIndexBuffer;
    nvrhi::BufferHandle m_box3DConstBuffer;
    nvrhi::GraphicsPipelineHandle m_box3DPipeline;
    nvrhi::BindingLayoutHandle m_box3DBindingLayout;
    nvrhi::BindingSetHandle m_box3DBindingSet;

    dm::int2 m_currMousePos = dm::int2::zero();

    // Inspector properties
    bool m_showTopo = false;
    SampleUtils::VolShadeType m_shadeType = SampleUtils::VolShadeType::VOXEL;

    float m_partSize = 100.f;
    float m_voxelSize = 0.5f;
    dm::float3 m_pivot;
};

class VoxelizeGUIPass : public donut::app::ImGuiRenderPass {
 public:
    VoxelizeGUIPass(donut::app::DeviceManager *deviceManager, VoxelizePass *attachPass)
        : ImGuiRenderPass{deviceManager}, m_attachPass{attachPass} {}

    bool Init() { return ImGuiRenderPass::Init(m_attachPass->m_shaderFactory); }

 protected:
    void BuildUI() override {
        if (ImGui::Begin("Inspector")) {
            ImGui::Checkbox("Show Topology", &m_attachPass->m_showTopo);

            if (ImGui::Combo("Shading Type", &m_shadeType,
                             "Voxel\0Surface\0Section\0Volume\0\0")) {
                switch (m_shadeType) {
                    default:
                    case 0:
                        m_attachPass->m_shadeType = gvdb::GVDBShadeType::VOXEL;
                        break;
                    case 1:
                        m_attachPass->m_shadeType = gvdb::GVDBShadeType::TRILINEAR;
                        break;
                    case 2:
                        m_attachPass->m_shadeType = gvdb::GVDBShadeType::SECTION3D;
                        break;
                    case 3:
                        m_attachPass->m_shadeType = gvdb::GVDBShadeType::VOLUME;
                        break;
                }
            }

            const char *items[] = {"0.5 mm", "0.4 mm", "0.3 mm", "0.2 mm"};
            if (ImGui::Combo("Voxel Size", &m_voxelSizeSelected, items, std::size(items))) {
                float voxelSize;
                switch (m_voxelSizeSelected) {
                    default:
                    case 0:
                        voxelSize = 0.5f;
                        break;
                    case 1:
                        voxelSize = 0.4f;
                        break;
                    case 2:
                        voxelSize = 0.3f;
                        break;
                    case 3:
                        voxelSize = 0.2f;
                        break;
                }
                m_attachPass->m_voxelSize = voxelSize;
                m_attachPass->Revoxelize();
            }

            ImGui::End();
        }
    }

    VoxelizePass *m_attachPass;
    int m_shadeType = 0;
    int m_voxelSizeSelected = 0;
};

int main(int argc, char *argv[]) {
    // donut::log::EnableOutputToConsole(true);
    donut::log::EnableOutputToDebug(true);
    donut::log::EnableOutputToMessageBox(false);

    // TODO(migration): dropped - ethereal::EnableCrtDumpHeapLeaks()
    // (<ethereal/core/object/UserAllocated.h>) exists in the old ethereal fork but not in donut.

    auto api = donut::app::GetGraphicsAPIFromCommandLine(argc, argv);
    auto deviceManager = donut::TakeOver(donut::app::DeviceManager::Create(api));

    donut::app::DeviceCreationParameters deviceParams;
#ifdef _DEBUG
    deviceParams.enableDebugRuntime = true;
    deviceParams.enableNvrhiValidationLayer = true;
    deviceParams.enableDebugRuntime = 1;
#endif
    deviceParams.swapChainFormat = nvrhi::Format::RGBA8_UNORM;
    // deviceParams.vsyncEnabled = true;

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, argv[0])) {
        donut::log::fatal(
            "Cannot initialize a graphics device with requested parameters");
        return -1;
    }
    {
        auto pass = donut::TakeOver(MAKE_RC_OBJ(VoxelizePass, deviceManager));
        if (!pass->Init()) {
            donut::log::error("VoxelizePass init failed");
            return -1;
        }
        deviceManager->AddRenderPassToBack(pass);

        auto imguiPass =
            donut::TakeOver(MAKE_RC_OBJ(VoxelizeGUIPass, deviceManager, pass));
        if (!imguiPass->Init()) {
            donut::log::error("VoxelizeGuiPass init failed");
            return -1;
        }
        deviceManager->AddRenderPassToBack(imguiPass);
    }

    deviceManager->RunMessageLoop();
    deviceManager->Shutdown();
    return 0;
}
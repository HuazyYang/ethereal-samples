#include <donut/app/DeviceManager.h>
#include <nvrhi/core/AutoPtr.h>
#include <donut/app/ApplicationBase.h>
#include <donut/core/log.h>
#include <nvrhi/core/Foundation.h>
#include <sample_utils/AppUtils.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/ShaderFactory.h>
#include <gvdb/GPDeviceCUDA.h>
#include <gvdb/GPDeviceNVRHI.h>
#include <gvdb/GVDBVoxelizer.h>
#include <gvdb/GVDBRenderer.h>
#include <gvdb/GVDBScene.h>
#include <gvdb/GVDBCamera.h>
#include <gvdb/GPDeviceNVRHI.h>
// NOTE(migration): countof() came from the old ethereal fork's <ethereal/core/math/basics.h>;
// donut has no equivalent, so std::size() is used instead.
#include <iterator>

struct SimpleCommon {
    dm::float4x4 modelMatrix;
    dm::float4x4 viewProjMatrix;
    dm::float4 eyePosW;
    dm::float4 lightPosW;
    dm::float4 ambientColor;
    dm::float4 diffuseFactor;
    dm::float4 specularFactor;
};

NVRHI_CCLSID(DepthMapPass, "1d9c1a44-0af0-4810-80b6-8ae4f3bac3ce")
class DepthMapPass : public donut::app::IRenderPass {
    NVRHI_DECLARE_UUID_TRAITS(DepthMapPass)
public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(DepthMapPass)
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IObject)
    NVRHI_IMPLEMENTS_CLASS(DepthMapPass)
    NVRHI_IMPLEMENTS_ROUTE_PARENT(donut::app::IRenderPass)
    NVRHI_END_INTERFACE_TABLE()

 public:
    DepthMapPass(donut::app::DeviceManager *deviceManager): IRenderPass{deviceManager} {}

    bool Init() {
        m_translation = dm::float3{0.f, 0.f, -25.f};
        m_rotationEulerAngles = dm::float3{0.f};
        m_bias = 1.f;

        m_vfs = nvrhi::TakeOver(MAKE_RC_OBJ(donut::vfs::RootFileSystem));
        auto binaryPath = donut::app::GetDirectoryWithExecutable();
        m_vfs->mount("gvdb/kernels", binaryPath);
        m_vfs->mount("app/shaders",
                     binaryPath / "compiled_shaders" /
                         donut::app::GetShaderTypeName(GetDevice()->getGraphicsAPI()));
        m_vfs->mount("app/asset", binaryPath / "asset");

        m_shaderFactory =
            nvrhi::TakeOver(MAKE_RC_OBJ(donut::engine::ShaderFactory, GetDevice(), m_vfs, ""));

        donut::gp::CUDADeviceDesc cudaDeviceDesc = {};
        cudaDeviceDesc.messageCallback = MAKE_RC_OBJ(GPDeviceMessageCallback);
        GVDB_V_GP(donut::gp::createCUDADevice(cudaDeviceDesc, &m_gpDevice));
        cudaDeviceDesc.messageCallback->Release();

        GVDB_V_GP(
            donut::createGPAndNVRHIDevice(GetDevice(), m_gpDevice, &m_interopDevice));

        donut::gp::DeviceQueueDesc gpQueueDesc = {};
        gpQueueDesc.priority = donut::gp::DeviceQueuePriority::AboveNormal;
        GVDB_V_GP(m_gpDevice->createDeviceQueue(gpQueueDesc, &m_gpQueue));

        // TODO(migration): dropped - ETHEREAL_NEW exists in the old ethereal fork but not in donut
        m_voxelizer.reset(new gvdb::GVDBVoxelizer(m_gpDevice, m_vfs));
        m_volumeRenderer.reset(
            new gvdb::GVDBRenderer{m_gpDevice, m_voxelizer->getNamedKernels()});

        m_voxelizer->configAtlas({16u}, 1);

        m_scene = nvrhi::TakeOver(MAKE_RC_OBJ(gvdb::GVDBScene));
        auto model = nvrhi::TakeOver(MAKE_RC_OBJ(gvdb::GVDBModel));
        model->loadObj(m_vfs, "app/asset/lucy.obj", dm::float3::zero(), dm::quat{}, dm::float3{100.f});
        m_scene->addModel(model);

        if(!m_voxelizer->loadVBX(m_vfs, "app/asset/explosion.vbx", m_volumeRenderer.get())) {
            donut::log::error("Failed to load vbx file.");
            return false;
        }

        m_voxelizer->getGVDB()->updateApron();

        m_volumeRenderer->setSteps(0.5f, 16.f, 0.5f);
        m_volumeRenderer->setExtinct(-1.f, 1.f, 0.f);
        m_volumeRenderer->setVolumeRange(0.1f, 0.f, 0.5f);
        m_volumeRenderer->setCutoff(0.005f, 0.005f, 0.f);
        m_volumeRenderer->setBackgroundColor(dm::float4{0.1f, 0.2f, 0.4f, 1.f});
        m_volumeRenderer->setLinearTransferFunc(0.00f, 0.10f, dm::float4::zero(),
                                                dm::float4::zero());
        m_volumeRenderer->setLinearTransferFunc(
            0.10f, 0.50f, dm::float4{1.f, 1.f, 0.f, 0.1f}, dm::float4{1.f, 0.f, 0.f, 0.3f});
        m_volumeRenderer->setLinearTransferFunc(0.50f, 0.75f,
                                                dm::float4{1.f, 0.f, 0.f, 0.3f},
                                                dm::float4{0.2f, 0.2f, 0.2f, 0.1f});
        m_volumeRenderer->setLinearTransferFunc(0.75f, 1.f,
                                                dm::float4{0.2f, 0.2f, 0.2f, 0.1f},
                                                dm::float4{0.1f, 0.1f, 0.1f, 0.3f});
        m_volumeRenderer->commitTransferFunc();
        m_volumeRenderer->setTransform(dm::float3{-125.f, -160.f, -125.f}, dm::float3{0.5f},
                                       m_rotationEulerAngles, m_translation);
        m_volumeRenderer->setRayNormalBias(1.f);
        m_volumeRenderer->setShading(gvdb::GVDBShadeType::VOLUME);

        auto camera = MAKE_RC_OBJ(gvdb::GVDBCamera);
        camera->setProjectionRH(true);
        camera->setViewParamsSpherical(dm::float3{0.f}, 300.f, dm::radians(70.f),
                                       dm::radians(-40.f));
        camera->setProjectParams(dm::radians(50.f), 0.1f, 1000.f);
        m_scene->setCamera(camera);
        camera->Release();

        auto light = MAKE_RC_OBJ(gvdb::GVDBLight);
        light->setProjectionRH(true);
        light->setViewParamsSpherical(dm::float3{0.f}, 500.f, dm::radians(32.7f),
                                      dm::radians(299.f));
        m_scene->addLight(light);
        light->Release();

        CreateRenderPiplines();

        m_commandList = GetDevice()->createCommandList();

        m_commandList->open();
        CreateModelMeshBuffers();
        m_commandList->close();

        GetDevice()->executeCommandList(m_commandList);

        return true;
    }

    void CreateRenderPiplines() {
        // Recreate depth PSO
        {
            auto vs = m_shaderFactory->CreateShader("app/shaders/Simple.hlsl", "VSMain", {},
                                                    nvrhi::ShaderType::Vertex);
            auto ps = m_shaderFactory->CreateShader("app/shaders/Simple.hlsl", "PSMain", {},
                                                    nvrhi::ShaderType::Pixel);

            nvrhi::VertexAttributeDesc vertAttrDescs[] = {
                {"POSITION", nvrhi::Format::RGB32_FLOAT, 1, 0, 0, sizeof(dm::float3)},
                {"NORMAL", nvrhi::Format::RGB32_FLOAT, 1, 0, 0, sizeof(dm::float3)}};
            auto inputLayout =
                GetDevice()->createInputLayout(vertAttrDescs, std::size(vertAttrDescs), vs);

            if (!m_depthBindingLayout) {
                nvrhi::BindingLayoutDesc bindingDesc;
                bindingDesc.visibility =
                    nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
                bindingDesc.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
                m_depthBindingLayout = GetDevice()->createBindingLayout(bindingDesc);
            }

            nvrhi::GraphicsPipelineDesc psoDesc = {};
            psoDesc.primType = nvrhi::PrimitiveType::TriangleList;
            psoDesc.inputLayout = inputLayout;
            psoDesc.bindingLayouts = { m_depthBindingLayout };
            psoDesc.VS = vs;
            psoDesc.PS = ps;
            psoDesc.renderState.rasterState.depthClipEnable = true;
            psoDesc.renderState.rasterState.frontCounterClockwise = true;
            psoDesc.renderState.depthStencilState.depthFunc =
                nvrhi::ComparisonFunc::LessOrEqual;
            psoDesc.renderState.rasterState.scissorEnable = true;
            psoDesc.fbInfo.colorFormats = {nvrhi::Format::RGBA8_UNORM,
                                           nvrhi::Format::R32_FLOAT};
            psoDesc.fbInfo.depthFormat = nvrhi::Format::D24S8;

            m_depthPSO = GetDevice()->createGraphicsPipeline(psoDesc);
        }

        // Depth pipeline immutable resources
        {
            nvrhi::BufferDesc bufDesc;
            bufDesc.isConstantBuffer = true;
            bufDesc.byteSize = sizeof(SimpleCommon);
            bufDesc.maxVersions = GetDeviceManager()->GetBackBufferCount();
            bufDesc.isVolatile = true;
            m_constBuffer = GetDevice()->createBuffer(bufDesc);

            nvrhi::BindingSetDesc bindingSetDesc;
            bindingSetDesc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, m_constBuffer)};
            m_depthBindingSet =
                GetDevice()->createBindingSet(bindingSetDesc, m_depthBindingLayout);
        }

        // Fullscreen PSO
        {
            auto vs = m_shaderFactory->CreateShader("app/shaders/Fullscreen.hlsl", "VSMain",
                                                    {}, nvrhi::ShaderType::Vertex);
            auto ps = m_shaderFactory->CreateShader("app/shaders/Fullscreen.hlsl", "PSMain",
                                                    {{"HAS_TEXTURE_2", "1"}},
                                                    nvrhi::ShaderType::Pixel);

            if (!m_fullscreenBindingLayout) {
                nvrhi::BindingLayoutDesc bindingDesc;
                bindingDesc.visibility =
                    nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
                bindingDesc.bindings = {
                    nvrhi::BindingLayoutItem::PushConstants(0, sizeof(dm::float4)),
                    nvrhi::BindingLayoutItem::Sampler(0),
                    nvrhi::BindingLayoutItem::Texture_SRV(0),
                    nvrhi::BindingLayoutItem::Texture_SRV(1)};

                m_fullscreenBindingLayout = GetDevice()->createBindingLayout(bindingDesc);
            }

            nvrhi::GraphicsPipelineDesc psoDesc;
            psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
            psoDesc.VS = vs;
            psoDesc.PS = ps;
            psoDesc.renderState.depthStencilState.depthTestEnable = false;
            psoDesc.renderState.rasterState.scissorEnable = true;
            psoDesc.bindingLayouts = {m_fullscreenBindingLayout};
            psoDesc.fbInfo = GetDeviceManager()->GetFramebuffer(0)->getFramebufferInfo();

            m_fullscreenPSO = GetDevice()->createGraphicsPipeline(psoDesc);
        }

        // Fullscreen pipeline immutable resources
        {
            nvrhi::SamplerDesc samplerDesc;
            samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::ClampToEdge);
            m_fullscreenSampler = GetDevice()->createSampler(samplerDesc);
        }
    }

    void CreateRenderBuffers() {
        int winWidth, winHeight;
        GetDeviceManager()->GetWindowDimensions(winWidth, winHeight);

        // Create depth pass frame buffer
        {
            nvrhi::TextureDesc texDesc;
            texDesc.dimension = nvrhi::TextureDimension::Texture2D;
            texDesc.width = winWidth;
            texDesc.height = winHeight;
            texDesc.format = nvrhi::Format::RGBA8_UNORM;
            texDesc.isShaderResource = true;
            texDesc.isRenderTarget = true;
            texDesc.initialState = nvrhi::ResourceStates::RenderTarget;
            texDesc.keepInitialState = true;
            m_colorTexture = GetDevice()->createTexture(texDesc);

            texDesc.format = nvrhi::Format::R32_FLOAT;
            texDesc.isShaderResource = true;
            texDesc.isRenderTarget = true;
            texDesc.initialState = nvrhi::ResourceStates::RenderTarget;
            texDesc.keepInitialState = true;
            texDesc.sharedResourceFlags = nvrhi::SharedResourceFlags::Shared |
                                          nvrhi::SharedResourceFlags::Shared_NTHandle;
            m_depthColorTexture = GetDevice()->createTexture(texDesc);

            texDesc = {};
            texDesc.dimension = nvrhi::TextureDimension::Texture2D;
            texDesc.width = winWidth;
            texDesc.height = winHeight;
            texDesc.format = nvrhi::Format::D24S8;
            texDesc.isRenderTarget = true;
            texDesc.isShaderResource = false;
            texDesc.initialState = nvrhi::ResourceStates::DepthWrite;
            texDesc.keepInitialState = true;
            m_depthPassDepthTexture = GetDevice()->createTexture(texDesc);

            nvrhi::FramebufferDesc fbDesc;
            fbDesc.addColorAttachment(m_colorTexture);
            fbDesc.addColorAttachment(m_depthColorTexture);
            fbDesc.setDepthAttachment(m_depthPassDepthTexture);
            m_depthFramebuffer = GetDevice()->createFramebuffer(fbDesc);

            GVDB_V_GP(m_interopDevice->createGPTexture(m_depthColorTexture, &m_depthColorTextureGP));
            m_depthColorTextureMutexIndex = 0;

            donut::gp::BufferDesc bufDesc;
            bufDesc.byteSize = (winWidth * winHeight * 4);
            GVDB_V_GP(m_gpDevice->createBuffer(bufDesc, &m_depthBufferGP));
        }

        // Recreate volume render buffer
        {
            donut::gp::BufferDesc bufDesc;
            bufDesc.byteSize = (winWidth * winHeight * 4);
            GVDB_V_GP(m_gpDevice->createBuffer(bufDesc, &m_volumeRenderBufferGP));

            nvrhi::TextureDesc texDesc;
            texDesc.dimension = nvrhi::TextureDimension::Texture2D;
            texDesc.width = winWidth;
            texDesc.height = winHeight;
            texDesc.format = nvrhi::Format::RGBA8_UNORM;
            texDesc.isShaderResource = true;
            texDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            texDesc.keepInitialState = true;
            texDesc.sharedResourceFlags = nvrhi::SharedResourceFlags::Shared |
                                          nvrhi::SharedResourceFlags::Shared_NTHandle;
            m_volumeRenderTexture = GetDevice()->createTexture(texDesc);

            GVDB_V_GP(m_interopDevice->createGPTexture(m_volumeRenderTexture,
                                                       &m_volumeRenderTextureGP));
            m_volumeRenderTextureMutexIndex = 0;
        }

        // Fullscreen binding set
        {
            nvrhi::BindingSetDesc bindingSetDesc;
            bindingSetDesc.bindings = {
                nvrhi::BindingSetItem::PushConstants(0, sizeof(dm::float4)),
                nvrhi::BindingSetItem::Sampler(0, m_fullscreenSampler),
                nvrhi::BindingSetItem::Texture_SRV(0, m_colorTexture),
                nvrhi::BindingSetItem::Texture_SRV(1, m_volumeRenderTexture)};
            m_fullscreenBindingSet =
                GetDevice()->createBindingSet(bindingSetDesc, m_fullscreenBindingLayout);
        }
    }

    void CreateModelMeshBuffers() {

        auto vertRange = m_scene->getModel(0)->getPositionBuffer();
        auto indexRange = m_scene->getModel(0)->getIndexBuffer();

        nvrhi::BufferDesc bufDesc;
        bufDesc.isVertexBuffer = true;
        bufDesc.byteSize = vertRange.size() * sizeof(dm::float3);
        bufDesc.initialState = nvrhi::ResourceStates::CopyDest;
        m_modelVertexBuffer = GetDevice()->createBuffer(bufDesc);
        m_commandList->beginTrackingBufferState(m_modelVertexBuffer, nvrhi::ResourceStates::CopyDest);
        m_commandList->writeBuffer(m_modelVertexBuffer, vertRange.data(), bufDesc.byteSize);
        m_commandList->setPermanentBufferState(m_modelVertexBuffer, nvrhi::ResourceStates::VertexBuffer);

        bufDesc.isVertexBuffer = false;
        bufDesc.isIndexBuffer = true;
        bufDesc.byteSize = indexRange.size() * sizeof(dm::int3);
        m_modelIndexBuffer = GetDevice()->createBuffer(bufDesc);
        m_commandList->beginTrackingBufferState(m_modelIndexBuffer, nvrhi::ResourceStates::CopyDest);
        m_commandList->writeBuffer(m_modelIndexBuffer, indexRange.data(), bufDesc.byteSize);
        m_commandList->setPermanentBufferState(m_modelIndexBuffer,
                                               nvrhi::ResourceStates::IndexBuffer);
        m_commandList->commitBarriers();
    }


    void BackBufferResized(const uint32_t width, const uint32_t height, const uint32_t sampleCount) override {
        m_scene->getCamera()->setWindowParams(0, 0, width, height);
        m_scene->getCamera()->setWindowParams(0, 0, width, height);

        CreateRenderBuffers();
    }

    void Animate(float fElapsedTimeseconds) override {
        m_scene->getCamera()->update();
        m_scene->getLight(0)->update();
    }

    void Render(nvrhi::IFramebuffer *fb) override {
        const auto &fbInfo = fb->getFramebufferInfo();

        // Raster depth pass
        m_interopDevice->commitNVRHIQueueWait(nvrhi::CommandQueue::Graphics);
        {
            m_commandList->open();
            m_interopDevice->acquireNVRHITextureKeyedMutex(
                m_depthColorTexture, m_depthColorTextureMutexIndex++, ~0);

            m_commandList->clearTextureFloat(m_colorTexture, nvrhi::AllSubresources,
                                             nvrhi::Color{0.f});
            m_commandList->clearTextureFloat(m_depthColorTexture, nvrhi::AllSubresources,
                                             nvrhi::Color{1.f});
            m_commandList->clearDepthStencilTexture(m_depthPassDepthTexture, nvrhi::AllSubresources, true, 1.f, true, 0);

            SimpleCommon constData;
            constData.modelMatrix = dm::float4x4::identity();
            constData.viewProjMatrix = m_scene->getCamera()->getViewMatrix() *
                                       m_scene->getCamera()->getProjMatrix();
            constData.eyePosW = dm::float4{m_scene->getCamera()->getCameraPos(), 1.f};
            constData.ambientColor = dm::float4{0.1f, 0.1f, 0.1f, 1.f};
            constData.diffuseFactor = dm::float4{0.5f, 0.5f, 0.5f, 1.f};
            constData.specularFactor = dm::float4{1.f};
            m_commandList->writeBuffer(m_constBuffer, &constData, sizeof(constData));

            nvrhi::GraphicsState state;
            state.framebuffer = m_depthFramebuffer;
            state.pipeline = m_depthPSO;
            state.bindings = {m_depthBindingSet};
            state.viewport.viewports = {
                nvrhi::Viewport{float(fbInfo.width), float(fbInfo.height)},
            };
            state.viewport.scissorRects = {
                nvrhi::Rect{int32_t(fbInfo.width), int32_t(fbInfo.height)}};
            state.vertexBuffers = {
                nvrhi::VertexBufferBinding{m_modelVertexBuffer, 0, 0},
            };
            state.indexBuffer =
                nvrhi::IndexBufferBinding{m_modelIndexBuffer, nvrhi::Format::R32_UINT, 0};
            m_commandList->setGraphicsState(state);

            nvrhi::DrawArguments drawArgs;
            drawArgs.vertexCount = m_scene->getModel(0)->getIndexBuffer().size() * 3;
            m_commandList->drawIndexed(drawArgs);

            m_interopDevice->releaseNVRHITextureKeyedMutex(m_depthColorTexture,
                                                           m_depthColorTextureMutexIndex);
            m_commandList->close();
            GetDevice()->executeCommandList(m_commandList);
        }
        m_interopDevice->commitNVRHIQueueSignal(nvrhi::CommandQueue::Graphics);

        // Volume render pass
        m_interopDevice->commitGPQueueWait(m_gpQueue, nvrhi::CommandQueue::Graphics);
        {
            donut::gp::GraphicsInteropKeyedMutexWaitParams gpResourceWaitParamSet[2] = {
                {m_depthColorTextureGP, m_depthColorTextureMutexIndex++, ~0u},
                {m_volumeRenderTextureGP, m_volumeRenderTextureMutexIndex++, ~0u}};
            m_interopDevice->acquireGPResourceKeyedMutexes(m_gpQueue,
                                                           gpResourceWaitParamSet, 2);

            // transfer depth buffer
            donut::gp::TextureCopyLocation srcLocation;
            srcLocation.type = donut::gp::TextureCopyType::SubresourceIndex;
            srcLocation.resource = m_depthColorTextureGP;
            srcLocation.subresourceIndex = 0;
            donut::gp::TextureCopyLocation dstLocation = {};
            dstLocation.type = donut::gp::TextureCopyType::PlacedFootprint;
            dstLocation.resource = m_depthBufferGP;
            dstLocation.placeFootprint.format = donut::gp::Format::R32_FLOAT;
            dstLocation.placeFootprint.width = fbInfo.width;
            dstLocation.placeFootprint.height = fbInfo.height;
            dstLocation.placeFootprint.rowPitch = fbInfo.width * 4;
            dstLocation.placeFootprint.depth = 1;
            m_gpQueue->copyTextureRegion(dstLocation, 0, 0, 0, srcLocation, nullptr);

            m_volumeRenderer->setDepthBuffer(m_depthBufferGP);
            m_volumeRenderer->render(m_voxelizer->getGVDB(), 0, m_volumeRenderBufferGP,
                                     m_scene->getCamera(), fbInfo.width, fbInfo.height,
                                     m_scene->getLight(0));

            // Copy GVDB output buffer into graphics texture
            srcLocation = {};
            srcLocation.type = donut::gp::TextureCopyType::PlacedFootprint;
            srcLocation.resource = m_volumeRenderBufferGP;
            srcLocation.placeFootprint.format = donut::gp::Format::RGBA8_UINT;
            srcLocation.placeFootprint.width = fbInfo.width;
            srcLocation.placeFootprint.height = fbInfo.height;
            srcLocation.placeFootprint.depth = 1;
            srcLocation.placeFootprint.rowPitch = fbInfo.width * 4;
            dstLocation = {};
            dstLocation.type = donut::gp::TextureCopyType::SubresourceIndex;
            dstLocation.resource = m_volumeRenderTextureGP;
            dstLocation.subresourceIndex = 0;
            m_gpQueue->copyTextureRegion(dstLocation, 0, 0, 0, srcLocation, nullptr);

            donut::gp::GraphicsInteropKeyedMutexSignalParams
                gpResourceSignalParamSet[2] = {
                    {m_depthColorTextureGP, m_depthColorTextureMutexIndex},
                    {m_volumeRenderTextureGP, m_volumeRenderTextureMutexIndex}};
            m_interopDevice->releaseGPResourceKeyedMutexes(m_gpQueue,
                                                           gpResourceSignalParamSet, 2);
        }
        m_interopDevice->commitGPQueueSignal(m_gpQueue, nvrhi::CommandQueue::Graphics);

        // Composite pass
        m_interopDevice->commitNVRHIQueueWait(nvrhi::CommandQueue::Graphics);
        {
            m_commandList->open();
            m_interopDevice->acquireNVRHITextureKeyedMutex(
                m_volumeRenderTexture, m_volumeRenderTextureMutexIndex++, ~0);

            nvrhi::GraphicsState state;
            state.framebuffer = fb;
            state.pipeline = m_fullscreenPSO;
            state.bindings = {m_fullscreenBindingSet};
            state.viewport.viewports = {
                nvrhi::Viewport{float(fbInfo.width), float(fbInfo.height)}};
            state.viewport.scissorRects = {
                nvrhi::Rect{int32_t(fbInfo.width), int32_t(fbInfo.height)}};
            m_commandList->setGraphicsState(state);

            dm::float4 constData = {1.f, 1.f, 0.f, 0.f};
            m_commandList->setPushConstants(&constData, sizeof(constData));

            nvrhi::DrawArguments drawArgs;
            drawArgs.vertexCount = 4;
            m_commandList->draw(drawArgs);

            m_commandList->close();
            m_interopDevice->releaseNVRHITextureKeyedMutex(m_volumeRenderTexture,
                                                           m_volumeRenderTextureMutexIndex);
            GetDevice()->executeCommandList(m_commandList);
        }
        m_interopDevice->commitNVRHIQueueSignal(nvrhi::CommandQueue::Graphics);
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
        return false;
    }

 private:
    nvrhi::AutoPtr<donut::vfs::RootFileSystem> m_vfs;
    nvrhi::AutoPtr<donut::engine::ShaderFactory> m_shaderFactory;
    nvrhi::AutoPtr<donut::gp::IDevice> m_gpDevice;
    nvrhi::AutoPtr<donut::gp::IDeviceQueue> m_gpQueue;
    nvrhi::AutoPtr<donut::IGPAndNVRHIInteropDevice> m_interopDevice;
    nvrhi::CommandListHandle m_commandList;

    std::unique_ptr<gvdb::GVDBVoxelizer> m_voxelizer;
    std::unique_ptr<gvdb::GVDBRenderer> m_volumeRenderer;
    nvrhi::AutoPtr<gvdb::GVDBScene> m_scene;

    dm::float3 m_rotationEulerAngles;
    dm::float3 m_translation;
    float m_bias;

    nvrhi::GraphicsPipelineHandle m_depthPSO;
    nvrhi::BindingLayoutHandle m_depthBindingLayout;
    nvrhi::GraphicsPipelineHandle m_fullscreenPSO;
    nvrhi::BindingLayoutHandle m_fullscreenBindingLayout;

    nvrhi::FramebufferHandle m_depthFramebuffer;
    nvrhi::TextureHandle m_colorTexture;
    nvrhi::TextureHandle m_depthColorTexture;
    nvrhi::TextureHandle m_depthPassDepthTexture;

    nvrhi::BindingSetHandle m_depthBindingSet;
    nvrhi::BufferHandle m_constBuffer;
    nvrhi::BufferHandle m_modelVertexBuffer;
    nvrhi::BufferHandle m_modelIndexBuffer;
    nvrhi::BindingSetHandle m_fullscreenBindingSet;
    nvrhi::SamplerHandle m_fullscreenSampler;

    nvrhi::AutoPtr<donut::gp::ITexture> m_depthColorTextureGP;
    uint32_t m_depthColorTextureMutexIndex = 0;
    nvrhi::AutoPtr<donut::gp::IBuffer> m_depthBufferGP;

    nvrhi::AutoPtr<donut::gp::IBuffer> m_volumeRenderBufferGP;
    nvrhi::TextureHandle m_volumeRenderTexture;
    nvrhi::AutoPtr<donut::gp::ITexture> m_volumeRenderTextureGP;
    uint32_t m_volumeRenderTextureMutexIndex = 0;
};

int main(int argc, char *argv[]) {
    // donut::log::EnableOutputToDebug(true);
    // donut::log::EnableOutputToMessageBox(false);
    // TODO(migration): dropped - ethereal::EnableCrtDumpHeapLeaks()
    // (<ethereal/core/object/UserAllocated.h>) exists in the old ethereal fork but not in donut.

    nvrhi::GraphicsAPI api = donut::app::GetGraphicsAPIFromCommandLine(argc, argv);
    auto deviceManager = nvrhi::TakeOver(donut::app::DeviceManager::Create(api));

    donut::app::DeviceCreationParameters deviceParams = {};
#ifdef _DEBUG
    deviceParams.enableDebugRuntime = true;
    deviceParams.enableNvrhiValidationLayer = true;
    deviceParams.enableDebugRuntime = true;
#endif
    deviceParams.swapChainFormat = nvrhi::Format::RGBA8_UNORM;

    if(!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, argv[0])) {
        donut::log::fatal(
            "Cannot initialize a graphics device with requested parameters");
        return -1;
    }

    auto pass = nvrhi::TakeOver(MAKE_RC_OBJ(DepthMapPass, deviceManager));
    if(!pass->Init()) {
        donut::log::fatal("Failed to init DepthMapPass");
        return -1;
    }
    deviceManager->AddRenderPassToBack(pass);
    pass = nullptr;

    deviceManager->RunMessageLoop();
    deviceManager->Shutdown();
    return 0;
}
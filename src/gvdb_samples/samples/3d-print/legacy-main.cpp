// 3d-print (g3DPrint): solid voxelization of lucy.obj at a selectable voxel
// size (0.5 / 0.4 / 0.3 / 0.2 mm for a 100 mm part), rendered with the CUDA
// raycaster; a 256x256 cross-section inset (Section2D) in the bottom-left
// quarter whose height follows the mouse; optional topology overlay.
#include <sample-utils/GVDBApp.h>
#include <sample-utils/ObjMesh.h>
#include <gvdb/GVDB.h>
#include <donut/core/log.h>
#include <imgui.h>

using namespace donut;
using namespace SampleUtils;

class PrintApp : public GVDBApp {
    NVRHI_INHERIT_INTERFACE_TABLE()
 public:
    using GVDBApp::GVDBApp;

    bool OnInit() override {
        log::info("Loading polygon model.");
        m_mesh = loadObjMesh(nullptr, getAssetPath("lucy.obj"), 1.f);
        if (!m_mesh) return false;
        auto meshInstance = MAKE_RC_OBJ_PTR(engine::MeshInstance, m_mesh);
        getSceneGraph()->AttachLeafNode(getSceneGraph()->GetRootNode(), meshInstance)->SetName("lucy");
        m_meshGP = uploadMeshGP(getGPDevice(), getGPQueue(), m_mesh);
        m_pivot = {0.3f, 0.45f, 0.3f};   // centre of the polygon model

        log::info("Configure.");
        m_volume = createVolume();
        if (!m_volume) return false;
        m_volume->configure(gvdb::GVDBLevelConfig::fromBranching(3, 3, 3, 3, 5));
        gvdb::GVDBAtlasConfig atlas;
        atlas.gridDim = {16u, 16u, 1u};
        m_volume->configureAtlas(atlas);
        if (NVRHI_FAILED(gvdb::createGVDBVoxelizer(m_volume, &m_voxelizer))) return false;
        if (NVRHI_FAILED(gvdb::createGVDBVoxelOps(m_volume, &m_ops))) return false;

        m_instance = addVolumeInstance(m_volume, dm::scaling(dm::float3(m_voxelSize)), "print");
        gvdb::VolumeRenderAttributes &attrs = m_instance->GetRenderAttributes();
        attrs.channel = 0;
        attrs.steps = {0.5f, 16.f, 0.5f};
        attrs.threshold = {0.25f, 0.f, 1.f};
        attrs.extinct = {-1.f, 1.1f, 0.f};
        attrs.cutoff = {0.005f, 0.005f, 0.f};
        attrs.transferFunction = MAKE_RC_OBJ_PTR(gvdb::VolumeTransferFunction, getGPDevice());
        attrs.transferFunction->setLinear(0.0f, 0.5f, {0, 0, 0, 0}, {1.f, 1.f, 1.f, 0.5f});
        attrs.transferFunction->setLinear(0.5f, 1.0f, {1.f, 1.f, 1.f, 0.5f}, {1.f, 1.f, 1.f, 0.8f});
        attrs.transferFunction->commit(getGPQueue());

        revoxelize();

        m_renderer = std::make_unique<VolRenderer>(getGPDevice(), getGPQueue(), getVFS());
        m_renderer->getViewParams().backgroundColor = {0.1f, 0.2f, 0.4f, 1.f};
        m_renderer->getViewParams().shadowParams = {0.f, 0.f, 0.f};

        getCamera()->verticalFov = verticalFovFromHorizontal(dm::radians(50.f), aspect());
        getCameraOrbit().setOrbit({-45.f, 30.f, 0.f}, m_pivot * m_partSize, 300.f);
        getLightOrbit().setOrbit({299.f, 57.3f, 0.f}, m_pivot * m_partSize * dm::float3(1.3f, 1.8f, 1.1f), 200.f);

        m_sectionTarget = getPresenter()->createTarget(256, 256);
        return true;
    }

    float aspect() const {
        uint32_t w = getPresenter()->getWidth(0), h = getPresenter()->getHeight(0);
        return h ? float(w) / float(h) : 4.f / 3.f;
    }

    void revoxelize() {
        static const float sizes[] = {0.5f, 0.4f, 0.3f, 0.2f};
        m_voxelSize = sizes[dm::clamp(m_voxelSizeSelect, 0, 3)];

        m_volume->destroyChannels();
        gvdb::GVDBChannelDesc chan;
        chan.format = gvdb::ATLAS_FORMAT_R32_FLOAT;
        m_volume->addChannel(chan);

        // Model -> index: rotate about the centre, move the pivot to the
        // origin corner, voxel size, part size (the reference's 1st .. 4th steps).
        dm::affine3 modelToIndex = dm::rotation(dm::float3(0.f, 1.f, 0.f), dm::radians(-10.f)) *
                                   dm::translation(m_pivot) * dm::scaling(dm::float3(1.f / m_voxelSize)) *
                                   dm::scaling(dm::float3(m_partSize));
        if (m_instance) setNodeTransform(m_instance->GetNode(), dm::scaling(dm::float3(m_voxelSize)));

        log::info("Voxelizing at %.1f mm.", m_voxelSize);
        nvrhi::FRESULT fr = m_voxelizer->solidVoxelize(0, m_meshGP.positions, m_meshGP.numVertices, m_meshGP.indices,
                                                       m_meshGP.numIndices, m_mesh->objectSpaceBounds, modelToIndex, 1.0f, 0.5f);
        if (NVRHI_FAILED(fr)) log::error("solidVoxelize failed (%d)", int(fr));
        m_ops->updateApron();
        m_volume->logMeasure();
        updateScene();
    }

    void OnRender(uint32_t width, uint32_t height) override {
        getCamera()->verticalFov = verticalFovFromHorizontal(dm::radians(50.f), float(width) / float(height));
        updateScene();
        RenderView view = getRenderView(int(width), int(height));

        float h = float(height);
        float mouseY = getFrameCounter() <= 1 ? h * 0.5f : float(getMousePos().y);
        float yslice = 100.f - mouseY * 100.f / h;

        static const gvdb::VolumeShading shadings[] = {gvdb::VolumeShading::Voxel, gvdb::VolumeShading::Trilinear,
                                                       gvdb::VolumeShading::Section3D, gvdb::VolumeShading::Volume};
        m_instance->GetRenderAttributes().shading = shadings[dm::clamp(m_shadeStyle, 0, 3)];

        // Main view: cross-section plane for Section3D at the mouse height.
        VolumeViewParams &params = m_renderer->getViewParams();
        params.sectionPoint = {0.f, yslice, 0.f};
        params.sectionNormal = {0.f, 1.f, 0.f};
        m_renderer->render(getSceneGraph(), view, getPresenter()->getRenderBuffer(0));

        // Inset: 2D section through the part at the mouse height.
        dm::float3 world = m_pivot * m_partSize / m_voxelSize;   // part centre in voxels
        world.y *= 1.f - mouseY / h;
        params.sectionPoint = world;
        params.sectionNormal = {world.x, 1.f, world.z};
        RenderView sectionView = getRenderView(256, 256);
        m_renderer->renderInstance(m_instance, sectionView, getPresenter()->getRenderBuffer(m_sectionTarget), false,
                                   gvdb::VolumeShading::Section2D);

        if (m_showTopology) getDebugDraw()->topology(m_instance);
    }

    void OnDrawOverlay(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer) override {
        getPresenter()->blit(commandList, framebuffer, m_sectionTarget, dm::box2(dm::float2(0.f, 0.75f), dm::float2(0.25f, 1.f)));
    }

    void OnBuildUI() override {
        ImGui::Checkbox("Topology", &m_showTopology);
        ImGui::Combo("Shading", &m_shadeStyle, "Voxel\0Surface\0Section\0Volume\0\0");
        if (ImGui::Combo("Voxel Size", &m_voxelSizeSelect, "0.5 mm, 10 MB\0" "0.4 mm, 10 MB\0" "0.3 mm, 20 MB\0" "0.2 mm, 40 MB\0\0"))
            revoxelize();
        ImGui::Text("Left: orbit, Middle: pan, Right: distance, Shift: light");
        ImGui::Text("Mouse height selects the cross-section");
    }

    bool OnKey(int key, int scancode, int action, int mods) override {
        if (action != GLFW_PRESS) return false;
        switch (key) {
            case GLFW_KEY_1: m_showTopology = !m_showTopology; return true;
            case GLFW_KEY_2: m_shadeStyle = (m_shadeStyle + 1) % 4; return true;
            default: return false;
        }
    }

 private:
    nvrhi::AutoPtr<engine::MeshInfo> m_mesh;
    MeshGPBuffers m_meshGP;
    nvrhi::AutoPtr<gvdb::IGVDBVolume> m_volume;
    nvrhi::AutoPtr<gvdb::IGVDBVoxelizer> m_voxelizer;
    nvrhi::AutoPtr<gvdb::IGVDBVoxelOps> m_ops;
    nvrhi::AutoPtr<gvdb::GVDBVolumeInstance> m_instance;
    std::unique_ptr<VolRenderer> m_renderer;
    int m_sectionTarget = -1;
    dm::float3 m_pivot = dm::float3::zero();
    float m_partSize = 100.f;   // part size in mm
    float m_voxelSize = 0.5f;
    int m_voxelSizeSelect = 0;
    int m_shadeStyle = 0;
    bool m_showTopology = false;
};

int main(int argc, const char **argv) {
    GVDBAppOptions options;
    options.title = "GVDB Voxels - 3d-print";
    options.sampleName = GVDB_SAMPLE_NAME;
    return runGVDBApp<PrintApp>(argc, argv, options);
}

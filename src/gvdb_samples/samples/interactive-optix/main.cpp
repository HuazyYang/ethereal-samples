// interactive-optix (gInteractiveOptix): explosion.vbx as one volume instance
// and lucy.obj as a mesh instance, rendered progressively with the OptiX
// renderer (OptixRenderer): the volume is a custom primitive intersected by
// the GVDB raycaster, the polygons a triangle GAS, both in one IAS. The
// shading of the volume (deep volume / trilinear surface / empty skipping /
// level set) selects the intersection program and the material, as the
// reference's RebuildOptixGraph did. Mouse: GVDBApp orbit controls (left:
// orbit, middle: pan, right: distance, shift: light); the volume is moved
// from the Inspector. Command line: --shading volume|trilinear|emptyskip|levelset
// selects the initial shading; --screenshot <file.png> [--samples N] saves the
// image after N (default 64) samples and exits.
#include <sample-utils/GVDBApp.h>
#include <sample-utils/OptixRenderer.h>
#include <sample-utils/ObjMesh.h>
#include <sample-utils/SampleTypes.h>
#include <sample-utils/ImageIO.h>
#include <gvdb/GVDB.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace donut;
using namespace SampleUtils;

class InteractiveOptixApp : public GVDBApp {
    NVRHI_INHERIT_INTERFACE_TABLE()
 public:
    using GVDBApp::GVDBApp;

    // --screenshot: save the output after `samples` samples and close.
    void setScreenshot(const std::string &path, int samples) {
        m_screenshotPath = path;
        m_screenshotSamples = samples;
    }
    void setInitialShading(gvdb::VolumeShading shading) { m_shading = shading; }

    bool OnInit() override {
        if (!getRTDevice()) {
            log::error("interactive-optix: no OptiX device (OptiX SDK not found at build time or no driver support).");
            return false;
        }

        // ---- polygon model ----
        log::info("Loading polygon model.");
        m_mesh = loadObjMesh(nullptr, getAssetPath("lucy.obj"), 100.f);
        if (!m_mesh) return false;
        m_meshInstance = MAKE_RC_OBJ_PTR(engine::MeshInstance, m_mesh);
        getSceneGraph()->AttachLeafNode(getSceneGraph()->GetRootNode(), m_meshInstance)->SetName("lucy");

        // ---- volume ----
        log::info("Loading VBX.");
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
        m_volume->logMeasure();
        if (NVRHI_FAILED(gvdb::createGVDBVoxelOps(m_volume, &m_ops))) return false;
        m_ops->updateApron();

        // Volume params of the reference: SetTransform(-125,-160,-125 / .25 / 0 / translate),
        // SetEpsilon, SetSteps, SetExtinct, SetVolumeRange, SetCutoff, LinearTransferFunc.
        m_instance = addVolumeInstance(m_volume, volumeTransform(), "explosion");
        gvdb::VolumeRenderAttributes &attrs = m_instance->GetRenderAttributes();
        attrs.channel = 0;
        attrs.steps = {0.5f, 16.f, 0.5f};
        attrs.extinct = {-0.25f, 1.0f, 0.0f};
        attrs.threshold = {0.1f, 0.0f, 0.3f};
        attrs.cutoff = {0.001f, 0.001f, 0.0f};
        attrs.epsilon = 0.001f;
        attrs.transferFunction = MAKE_RC_OBJ_PTR(gvdb::VolumeTransferFunction, getGPDevice());
        attrs.transferFunction->setLinear(0.00f, 0.25f, {0.f, 0.f, 0.f, 0.0f}, {0.f, 0.f, 0.f, 0.0f});
        attrs.transferFunction->setLinear(0.25f, 0.50f, {0.f, 0.5f, 1.f, 0.0f}, {0.f, 0.f, 1.f, 0.2f});
        attrs.transferFunction->setLinear(0.50f, 0.75f, {0.f, 0.f, 1.f, 0.2f}, {0.f, 1.f, 1.f, 0.3f});
        attrs.transferFunction->setLinear(0.75f, 1.00f, {0.f, 1.f, 1.f, 0.3f}, {0.f, 1.f, 1.f, 1.0f});
        attrs.transferFunction->commit(getGPQueue());

        // ---- OptiX renderer and materials (RebuildOptixGraph of the reference) ----
        m_renderer = std::make_unique<OptixRenderer>(getGPDevice(), getRTDevice(), getGPQueue(), getVFS());
        m_renderer->getViewParams().backgroundColor = {0.1f, 0.2f, 0.4f, 1.f};

        OptixMaterialParams surf;
        surf.lightWidth = 0.5f;
        surf.shadowWidth = 0.5f;
        surf.diffColor = {.5f, .54f, .5f};
        surf.specColor = {.7f, .7f, .7f};
        surf.specPower = 80.f;
        surf.envColor = {0.f, 0.f, 0.f};
        surf.reflWidth = 0.3f;
        surf.reflColor = {.8f, .8f, .8f};
        surf.refrWidth = 0.f;
        surf.refrColor = {0.f, 0.f, 0.f};
        surf.refrIor = 1.2f;
        surf.refrAmount = 1.f;
        surf.refrOffset = 15.f;
        m_matSurface = m_renderer->addMaterial(surf);   // surface objects (and the polygons)
        m_matDeep = m_renderer->addMaterial(surf);      // volumetric objects
        m_renderer->setMeshMaterial(m_matSurface);
        applyShading();

        // ---- camera and light ----
        getCamera()->zNear = 0.1f;
        getCamera()->zFar = 5000.f;
        getCameraOrbit().setOrbit({-20.f, 30.f, 0.f}, {0.f, 0.f, 0.f}, 400.f);
        getLightOrbit().setOrbit({45.f, 45.f, 0.f}, {0.f, 0.f, 0.f}, 200.f);
        updateScene();

        log::info("Building the OptiX scene.");
        m_renderer->buildScene(getSceneGraph());
        return true;
    }

    dm::affine3 volumeTransform() const {
        return makeVolumeTransform({-125.f, -160.f, -125.f}, dm::float3(0.25f), dm::float3::zero(), m_translate);
    }

    // Shading -> intersection program and material of the volume instance.
    void applyShading() {
        gvdb::VolumeRenderAttributes &attrs = m_instance->GetRenderAttributes();
        attrs.shading = m_shading;
        attrs.materialId = (m_shading == gvdb::VolumeShading::Volume) ? m_matDeep : m_matSurface;
        m_sample = 0;
    }

    void OnRender(uint32_t width, uint32_t height) override {
        if (m_renderer->getWidth() != width || m_renderer->getHeight() != height) {
            m_renderer->resizeOutput(width, height);
            m_sample = 0;
        }
        getCamera()->verticalFov = verticalFovFromHorizontal(dm::radians(30.f), float(width) / float(height));

        if (m_transformDirty) {
            setNodeTransform(m_instance->GetNode(), volumeTransform());
            updateScene();
            m_renderer->updateTransforms(getSceneGraph());
            m_transformDirty = false;
            m_sample = 0;
        }

        // progressive sampling: sample 0 restarts the accumulation
        m_renderer->setSample(m_frame, m_sample);
        m_renderer->render(getSceneGraph(), getRenderView(int(width), int(height)));
        UT_V_GP(getGPQueue()->copyBufferRegion(getPresenter()->getRenderBuffer(0), 0, m_renderer->getOutputBuffer(), 0,
                                               size_t(width) * height * 4));

        if (!m_screenshotPath.empty() && m_frame == 0 && m_sample + 1 >= m_screenshotSamples) {
            std::vector<uint8_t> rgba;
            m_renderer->readOutput(rgba);
            if (savePNG(m_screenshotPath, rgba.data(), width, height))
                log::info("Saved %s (%d samples)", m_screenshotPath.c_str(), m_sample + 1);
            else
                log::error("Cannot write %s", m_screenshotPath.c_str());
            glfwSetWindowShouldClose(GetDeviceManager()->GetWindow(), GLFW_TRUE);
            m_screenshotPath.clear();
        }

        if (++m_sample >= m_maxSamples) {
            ++m_frame;
            m_sample = 0;
        }
    }

    void OnMouseDrag(int button, int dx, int dy, int mods) override {
        GVDBApp::OnMouseDrag(button, dx, dy, mods);
        m_sample = 0;   // camera or light moved
    }

    void OnBuildUI() override {
        static const char *shadingNames[] = {"Volume (deep)", "Trilinear surface", "Empty skipping", "Level set"};
        static const gvdb::VolumeShading shadingValues[] = {gvdb::VolumeShading::Volume, gvdb::VolumeShading::Trilinear,
                                                            gvdb::VolumeShading::EmptySkip, gvdb::VolumeShading::LevelSet};
        int current = 0;
        for (int i = 0; i < 4; ++i)
            if (shadingValues[i] == m_shading) current = i;
        if (ImGui::Combo("Shading", &current, shadingNames, 4)) {
            m_shading = shadingValues[current];
            applyShading();
        }
        if (ImGui::DragFloat3("Volume translate", &m_translate.x, 1.f)) m_transformDirty = true;
        ImGui::SliderInt("Max samples", &m_maxSamples, 1, 4096);
        ImGui::Text("Frame %d, sample %d / %d", m_frame, m_sample, m_maxSamples);
        ImGui::Text("Left: orbit, Middle: pan, Right: distance, Shift: light");
    }

 private:
    nvrhi::AutoPtr<engine::MeshInfo> m_mesh;
    nvrhi::AutoPtr<engine::MeshInstance> m_meshInstance;
    nvrhi::AutoPtr<gvdb::IGVDBVolume> m_volume;
    nvrhi::AutoPtr<gvdb::IGVDBVoxelOps> m_ops;
    nvrhi::AutoPtr<gvdb::GVDBVolumeInstance> m_instance;
    std::unique_ptr<OptixRenderer> m_renderer;
    gvdb::VolumeShading m_shading = gvdb::VolumeShading::Volume;
    int m_matSurface = 0;
    int m_matDeep = 0;
    dm::float3 m_translate = {44.f, 0.f, 16.f};
    bool m_transformDirty = false;
    int m_frame = 0;
    int m_sample = 0;
    int m_maxSamples = 1024;
    std::string m_screenshotPath;
    int m_screenshotSamples = 64;
};

int main(int argc, const char **argv) {
    std::string screenshot;
    int screenshotSamples = 64;
    gvdb::VolumeShading shading = gvdb::VolumeShading::Volume;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot = argv[++i];
        } else if (strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
            screenshotSamples = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--shading") == 0 && i + 1 < argc) {
            const char *name = argv[++i];
            if (strcmp(name, "trilinear") == 0) shading = gvdb::VolumeShading::Trilinear;
            else if (strcmp(name, "emptyskip") == 0) shading = gvdb::VolumeShading::EmptySkip;
            else if (strcmp(name, "levelset") == 0) shading = gvdb::VolumeShading::LevelSet;
            else shading = gvdb::VolumeShading::Volume;
        }
    }

    GVDBAppOptions options;
    options.title = "GVDB Voxels - interactive-optix";
    options.sampleName = GVDB_SAMPLE_NAME;
    options.wantOptix = true;
    return runGVDBApp(argc, argv, options, [&](app::DeviceManager *dm) -> nvrhi::AutoPtr<GVDBApp> {
        auto app = MAKE_RC_OBJ_PTR(InteractiveOptixApp, dm);
        app->setInitialShading(shading);
        if (!screenshot.empty()) app->setScreenshot(screenshot, screenshotSamples);
        return app;
    });
}

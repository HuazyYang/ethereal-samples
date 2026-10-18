// spray-deposit (gSprayDeposit): metal.obj is voxelized (density + colour
// channel); every frame a bundle of rays from a moving wand is traced against
// the volume (VolRenderer::raytrace), the hit points are inserted as points
// and gathered into the density / colour channels (material deposition), with
// periodic smoothing. The rays are drawn with DebugDraw.
#include <sample-utils/GVDBApp.h>
#include <sample-utils/ObjMesh.h>
#include <gvdb/GVDB.h>
#include <donut/core/log.h>
#include <imgui.h>
#include <random>

using namespace donut;
using namespace SampleUtils;

enum WandStyle { WAND_ROTATE = 0, WAND_SWEEP = 1, WAND_WAVE = 2 };

class SprayApp : public GVDBApp {
    NVRHI_INHERIT_INTERFACE_TABLE()
 public:
    using GVDBApp::GVDBApp;

    bool OnInit() override {
        log::info("Loading polygon model.");
        m_mesh = loadObjMesh(nullptr, getAssetPath("metal.obj"), 1.f);
        if (!m_mesh) return false;
        auto meshInstance = MAKE_RC_OBJ_PTR(engine::MeshInstance, m_mesh);
        getSceneGraph()->AttachLeafNode(getSceneGraph()->GetRootNode(), meshInstance)->SetName("metal");
        m_meshGP = uploadMeshGP(getGPDevice(), getGPQueue(), m_mesh);

        m_volume = createVolume();
        if (!m_volume) return false;
        m_volume->configure(gvdb::GVDBLevelConfig::fromBranching(3, 3, 3, 3, 5));
        gvdb::GVDBChannelDesc density;
        density.format = gvdb::ATLAS_FORMAT_R32_FLOAT;
        m_volume->addChannel(density);
        gvdb::GVDBChannelDesc color;
        color.format = gvdb::ATLAS_FORMAT_RGBA8_UINT;
        m_volume->addChannel(color);
        if (NVRHI_FAILED(gvdb::createGVDBVoxelizer(m_volume, &m_voxelizer))) return false;
        if (NVRHI_FAILED(gvdb::createGVDBVoxelOps(m_volume, &m_ops))) return false;
        if (NVRHI_FAILED(gvdb::createGVDBPointOps(m_volume, &m_points))) return false;

        m_instance = addVolumeInstance(m_volume, dm::affine3::identity(), "part");
        gvdb::VolumeRenderAttributes &attrs = m_instance->GetRenderAttributes();
        attrs.channel = 0;
        attrs.colorChannel = 1;   // channel 1 modulates the colour
        attrs.steps = {0.25f, 16.f, 0.25f};
        attrs.extinct = {-1.f, 1.2f, 0.f};
        attrs.threshold = {0.3f, 0.f, 1.f};
        attrs.cutoff = {0.005f, 0.01f, 0.f};
        attrs.epsilon = 0.01f;
        attrs.transferFunction = MAKE_RC_OBJ_PTR(gvdb::VolumeTransferFunction, getGPDevice());
        attrs.transferFunction->setLinear(0.0f, 0.2f, {0, 0, 0, 0}, {1, 1, 1, 0.1f});
        attrs.transferFunction->setLinear(0.2f, 0.3f, {1, 1, 1, 0.05f}, {1, 1, 1, 0.05f});
        attrs.transferFunction->setLinear(0.3f, 1.0f, {1, 1, 1, 0.05f}, {0, 0, 0, 0});
        attrs.transferFunction->commit(getGPQueue());

        m_renderer = std::make_unique<VolRenderer>(getGPDevice(), getGPQueue(), getVFS());
        m_renderer->getViewParams().backgroundColor = {0.1f, 0.2f, 0.4f, 1.f};

        getCameraOrbit().setOrbit({45.f, 40.f, 0.f}, {200.f, 200.f, 200.f}, 1000.f);
        getLightOrbit().setOrbit({80.f, 50.f, 0.f}, {200.f, 200.f, 200.f}, 800.f);

        // Ray bundle: host copy + device buffer (+ staging buffer for drawing).
        m_rays.resize(m_numRays);
        gp::BufferDesc desc;
        desc.byteSize = m_numRays * sizeof(ScnRay);
        UT_V_GP(getGPDevice()->createBuffer(desc, &m_raysGP));
        desc.isStaging = true;
        UT_V_GP(getGPDevice()->createBuffer(desc, &m_raysStaging));

        // Part: 200 mm, placed at (200, 200, 200) in voxels.
        float partSize = 200.f;
        dm::affine3 modelToIndex = dm::scaling(dm::float3(partSize)) * dm::translation(dm::float3(200.f));
        log::info("Voxelizing.");
        nvrhi::FRESULT fr = m_voxelizer->solidVoxelize(0, m_meshGP.positions, m_meshGP.numVertices, m_meshGP.indices,
                                                       m_meshGP.numIndices, m_mesh->objectSpaceBounds, modelToIndex, 1.f, 1.f);
        if (NVRHI_FAILED(fr)) log::error("solidVoxelize failed (%d)", int(fr));
        m_ops->fillChannel(1, {0.7f, 0.7f, 0.7f, 1.f});
        m_ops->compute(gvdb::ComputeOp::Smooth, 0, 2, {4.f, 0.f, 0.f}, true, true);
        m_ops->updateApron();
        m_volume->logMeasure();
        return true;
    }

    void setTransferForShading() {
        gvdb::VolumeTransferFunction *tf = m_instance->GetRenderAttributes().transferFunction;
        if (m_shadeStyle == 3) {   // cross-section style, orange border
            tf->setLinear(0.0f, 0.2f, {0, 0, 0, 0}, {1, 0.5f, 0, 0.5f});
            tf->setLinear(0.2f, 0.3f, {1, 0.5f, 0, 0.5f}, {0, 0, 0, 0});
            tf->setLinear(0.3f, 1.0f, {0, 0, 0, 0}, {0, 0, 0, 0});
        } else {   // volumetric style, x-ray white
            tf->setLinear(0.00f, 0.1f, {0, 0, 0, 0}, {0, 0, 0, 0});
            tf->setLinear(0.1f, 0.25f, {0, 0, 0, 0}, {1, 1, 1, 0.8f});
            tf->setLinear(0.25f, 0.5f, {1, 1, 1, 0.8f}, {0, 0, 0, 0});
            tf->setLinear(0.5f, 1.0f, {0, 0, 0, 0}, {0, 0, 0, 0});
        }
        tf->commit(getGPQueue());
    }

    void simulate(const RenderView &view) {
        m_time += 1.f;
        std::uniform_real_distribution<float> rnd(-1.f, 1.f);
        const int ndiv = int(sqrtf(float(m_numRays)));

        dm::float3 effect = dm::float3::zero();
        switch (m_wandStyle) {
            case WAND_ROTATE: effect = {1.f, 0.f, 0.f}; break;
            case WAND_SWEEP: effect = {0.f, 50.f, 0.f}; break;
            case WAND_WAVE: effect = {0.f, 0.f, 0.5f}; break;
        }
        dm::affine3 rot = dm::rotation(dm::float3(0.f, 1.f, 0.f), dm::radians(effect.x * m_time));   // wand rotation

        dm::float3 clr;   // colour variation
        clr.x = sinf(dm::radians(2.0f * m_time)) * 0.5f + 0.5f;
        clr.y = sinf(dm::radians(1.0f * m_time)) * 0.5f + 0.5f;
        clr.z = sinf(dm::radians(0.5f * m_time)) * 0.5f + 0.5f;

        float st = sinf(dm::radians(m_time));              // sin time
        float lt = (int(m_time) % 100) / 100.f - 0.5f;      // linear time

        for (int n = 0; n < m_numRays; ++n) {
            ScnRay &ray = m_rays[n];
            float x = float(n % ndiv) / ndiv - 0.5f + rnd(m_rng) * 0.1f;   // random variation in rays
            float y = float(n / ndiv) / ndiv - 0.5f + rnd(m_rng) * 0.1f;
            x = dm::clamp(x, -0.5f, 0.5f);
            y = dm::clamp(y, -0.5f, 0.5f);
            dm::float3 orig(x * 25.f, 0.f, y * 0.5f + lt * effect.y);   // wand sweeping
            orig = rot.transformVector(orig);                           // rotating wand over time
            orig += dm::float3(200.f, 400.f, 200.f);                    // position of wand
            dm::float3 dir(rnd(m_rng) * 0.1f, 0.f, 0.f);
            dir += dm::float3(x, -0.5f, y * 0.04f + st * effect.z);     // wand angle
            dir = dm::normalize(dir);
            dir = rot.transformVector(dir);
            ray.orig = orig;
            ray.dir = dir;
            ray.hit = {GVDB_NOHIT, GVDB_NOHIT, GVDB_NOHIT};
            ray.normal = dm::float3::zero();
            ray.clr = packColorA(clr.x, clr.y, clr.z, 0.2f);
            ray.pnode = ray.pndx = 0;
        }
        UT_V_GP(getGPQueue()->writeBuffer(m_raysGP, m_rays.data(), m_numRays * sizeof(ScnRay), 0));

        m_renderer->raytrace(m_instance, m_raysGP, uint32_t(m_numRays), -0.0001f, view);

        gvdb::PointData pos, vel, col;
        pos.buffer = m_raysGP;
        pos.offset = offsetof(ScnRay, hit);
        pos.stride = sizeof(ScnRay);
        pos.count = uint32_t(m_numRays);
        col = pos;
        col.offset = offsetof(ScnRay, clr);
        m_points->setPoints(pos, vel, col);

        int scPntLen = 0;
        const int subcellSize = 4;
        const float radius = 1.f;
        m_points->insertPointsSubcell(subcellSize, uint32_t(m_numRays), radius, dm::float3::zero(), scPntLen);
        m_points->gatherDensity(subcellSize, uint32_t(m_numRays), radius, dm::float3::zero(), scPntLen, 0, 1, true);
        m_ops->updateApron();

        if (int(m_time) % 20 == 0) m_ops->compute(gvdb::ComputeOp::Smooth, 0, 1, {4.f, 0.f, 0.f}, true, false);
    }

    void drawRays() {
        UT_V_GP(getGPQueue()->copyBufferRegion(m_raysStaging, 0, m_raysGP, 0, m_numRays * sizeof(ScnRay)));
        syncQueue(getGPDevice(), getGPQueue());
        void *data = nullptr;
        UT_V_GP(getGPDevice()->mapBuffer(m_raysStaging, &data));
        if (!data) return;
        const ScnRay *rays = static_cast<const ScnRay *>(data);
        dm::affine3 toWorld = m_instance->GetIndexToWorld();
        for (int n = 0; n < m_numRays; ++n) {
            const ScnRay &ray = rays[n];
            if (ray.hit.z == GVDB_NOHIT) continue;
            dm::float4 c(float(ray.clr & 255) / 255.f, float((ray.clr >> 8) & 255) / 255.f,
                         float((ray.clr >> 16) & 255) / 255.f, float((ray.clr >> 24) & 255) / 255.f);
            getDebugDraw()->line(toWorld.transformPoint(ray.orig), toWorld.transformPoint(ray.hit), c);
        }
        getGPDevice()->unmapBuffer(m_raysStaging);
    }

    void OnRender(uint32_t width, uint32_t height) override {
        getCamera()->verticalFov = verticalFovFromHorizontal(dm::radians(50.f), float(width) / float(height));
        updateScene();
        RenderView view = getRenderView(int(width), int(height));

        if (m_simulate) simulate(view);

        m_renderer->getViewParams().sectionPoint = {50.f, 50.f, 50.f};
        m_renderer->getViewParams().sectionNormal = {-1.f, 0.f, 0.f};
        static const gvdb::VolumeShading shadings[] = {gvdb::VolumeShading::Off, gvdb::VolumeShading::Voxel,
                                                       gvdb::VolumeShading::Trilinear, gvdb::VolumeShading::Section3D,
                                                       gvdb::VolumeShading::Volume};
        m_instance->GetRenderAttributes().shading = shadings[dm::clamp(m_shadeStyle, 0, 4)];
        m_renderer->render(getSceneGraph(), view, getPresenter()->getRenderBuffer(0));

        if (m_showRays && m_simulate) drawRays();
        if (m_showTopology) getDebugDraw()->topology(m_instance);
    }

    void OnBuildUI() override {
        ImGui::Checkbox("Simulate", &m_simulate);
        ImGui::Checkbox("Topology", &m_showTopology);
        ImGui::Checkbox("Rays", &m_showRays);
        if (ImGui::Combo("Shading", &m_shadeStyle, "Off\0Voxel\0Surface\0Section\0Volume\0\0")) setTransferForShading();
        ImGui::Combo("Wand Style", &m_wandStyle, "Rotate\0Sweep\0Wave\0\0");
    }

    bool OnKey(int key, int scancode, int action, int mods) override {
        if (action != GLFW_PRESS) return false;
        switch (key) {
            case GLFW_KEY_1:
            case GLFW_KEY_SPACE: m_simulate = !m_simulate; return true;
            case GLFW_KEY_2: m_showTopology = !m_showTopology; return true;
            case GLFW_KEY_3: m_showRays = !m_showRays; return true;
            case GLFW_KEY_4: m_shadeStyle = (m_shadeStyle + 1) % 5; setTransferForShading(); return true;
            case GLFW_KEY_5: m_wandStyle = (m_wandStyle + 1) % 3; return true;
            default: return false;
        }
    }

 private:
    nvrhi::AutoPtr<engine::MeshInfo> m_mesh;
    MeshGPBuffers m_meshGP;
    nvrhi::AutoPtr<gvdb::IGVDBVolume> m_volume;
    nvrhi::AutoPtr<gvdb::IGVDBVoxelizer> m_voxelizer;
    nvrhi::AutoPtr<gvdb::IGVDBVoxelOps> m_ops;
    nvrhi::AutoPtr<gvdb::IGVDBPointOps> m_points;
    nvrhi::AutoPtr<gvdb::GVDBVolumeInstance> m_instance;
    std::unique_ptr<VolRenderer> m_renderer;
    std::vector<ScnRay> m_rays;
    nvrhi::AutoPtr<gp::IBuffer> m_raysGP;
    nvrhi::AutoPtr<gp::IBuffer> m_raysStaging;
    std::mt19937 m_rng{6572};
    int m_numRays = 1000;
    float m_time = 0.f;
    bool m_simulate = true;
    bool m_showTopology = false;
    bool m_showRays = true;
    int m_shadeStyle = 2;
    int m_wandStyle = WAND_ROTATE;
};

int main(int argc, const char **argv) {
    GVDBAppOptions options;
    options.title = "GVDB Voxels - spray-deposit";
    options.sampleName = GVDB_SAMPLE_NAME;
    return runGVDBApp<SprayApp>(argc, argv, options);
}

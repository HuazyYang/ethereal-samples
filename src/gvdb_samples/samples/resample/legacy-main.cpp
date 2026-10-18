// resample (gResample): head.raw (128x256x256 bytes) converted to float and
// resampled into a GVDB topology that is either dense or sparse (activated
// from a downsampled copy, optionally with a halo of neighbouring bricks).
#include <sample-utils/GVDBApp.h>
#include <gvdb/GVDB.h>
#include <donut/core/log.h>
#include <imgui.h>
#include <fstream>

using namespace donut;
using namespace SampleUtils;

class ResampleApp : public GVDBApp {
    NVRHI_INHERIT_INTERFACE_TABLE()
 public:
    using GVDBApp::GVDBApp;

    bool loadRAW(const std::filesystem::path &path, dm::int3 res) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            log::error("Cannot read %s", path.string().c_str());
            return false;
        }
        size_t count = size_t(res.x) * res.y * res.z;
        std::vector<uint8_t> bytes(count);
        file.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(count));
        log::info("Convert to float.");
        std::vector<float> values(count);
        for (size_t i = 0; i < count; ++i) values[i] = float(bytes[i]) / 256.f;
        m_dataRes = res;

        log::info("Transfer data to GPU.");
        gp::BufferDesc desc;
        desc.byteSize = count * sizeof(float);
        UT_V_GP(getGPDevice()->createBuffer(desc, &m_data));
        UT_V_GP(getGPQueue()->writeBuffer(m_data, values.data(), desc.byteSize, 0));
        return true;
    }

    bool OnInit() override {
        log::info("Loading volume data.");
        if (!loadRAW(getAssetPath("head.raw"), {128, 256, 256})) return false;

        m_volume = createVolume();
        if (!m_volume) return false;
        if (NVRHI_FAILED(gvdb::createGVDBVoxelOps(m_volume, &m_ops))) return false;
        m_instance = addVolumeInstance(m_volume, dm::affine3::identity(), "head");

        m_volMax = {256.f, 256.f, 256.f};
        rebuild();

        gvdb::VolumeRenderAttributes &attrs = m_instance->GetRenderAttributes();
        attrs.channel = 0;
        attrs.shading = gvdb::VolumeShading::Volume;
        attrs.steps = {0.5f, 16.f, 0.5f};
        attrs.extinct = {-1.f, 1.5f, 0.f};
        attrs.threshold = {0.05f, 0.f, 1.f};
        attrs.cutoff = {0.001f, 0.001f, 0.f};
        attrs.epsilon = 0.01f;   // larger than the default to avoid artifacts between bricks
        attrs.transferFunction = MAKE_RC_OBJ_PTR(gvdb::VolumeTransferFunction, getGPDevice());
        attrs.transferFunction->setLinear(0.00f, 0.15f, {0, 0, 0, 0}, {0, 0, 0, 0});
        attrs.transferFunction->setLinear(0.15f, 0.25f, {0, 0, 0, 0}, {0, 0, 1, 0.01f});            // skin, blue
        attrs.transferFunction->setLinear(0.25f, 0.50f, {0, 0, 1, 0.01f}, {1, 0, 0, 0.02f});        // bone, red
        attrs.transferFunction->setLinear(0.50f, 0.75f, {1, 0, 0, 0.02f}, {0.2f, 0.2f, 0.2f, 0.02f});
        attrs.transferFunction->setLinear(0.75f, 1.00f, {0.2f, 0.2f, 0.2f, 0.02f}, {0.1f, 0.1f, 0.1f, 0.1f});
        attrs.transferFunction->commit(getGPQueue());

        m_renderer = std::make_unique<VolRenderer>(getGPDevice(), getGPQueue(), getVFS());
        m_renderer->getViewParams().backgroundColor = {0.1f, 0.2f, 0.4f, 1.f};

        getCamera()->zNear = 0.1f;
        getCamera()->zFar = 5000.f;
        getCameraOrbit().setOrbit({90.f, 20.f, 0.f}, {128.f, 100.f, 128.f}, 1000.f);
        getLightOrbit().setOrbit({299.f, 57.3f, 0.f}, {0.f, 0.f, 0.f}, 1400.f);
        return true;
    }

    // Index space -> source voxel of head.raw: the reference Matrix4F::SRT(
    // (1,0,0), (0,0,1), (0,1,0), t=(0,0,252), s=(.5,-1,1)), i.e. src = (0.5*x, z, 252 - y).
    static dm::float4x4 sourceTransform() {
        return dm::float4x4(dm::float4(0.5f, 0.f, 0.f, 0.f), dm::float4(0.f, 0.f, -1.f, 0.f),
                            dm::float4(0.f, 1.f, 0.f, 0.f), dm::float4(0.f, 0.f, 252.f, 1.f));
    }

    void rebuild() {
        m_volume->clear();
        m_volume->destroyChannels();

        log::info("Configure GVDB.");
        m_volume->configure(gvdb::GVDBLevelConfig::fromBranching(3, 3, 3, 3, 4));
        gvdb::GVDBAtlasConfig atlas;
        atlas.gridDim = {16u, 16u, 16u};
        m_volume->configureAtlas(atlas);
        gvdb::GVDBChannelDesc chan;
        chan.format = gvdb::ATLAS_FORMAT_R32_FLOAT;
        m_volume->addChannel(chan);

        dm::float4x4 xform = sourceTransform();
        const dm::float3 inRange(0.f, 1.f, 0.f), outRange(0.f, 1.f, 0.f);

        log::info("Activate GVDB volume.");
        gvdb::Extents e = m_volume->computeExtents(1, dm::box3(dm::float3::zero(), m_volMax));
        if (!m_sparse) {
            m_volume->activateRegion(e);
        } else {
            std::vector<float> downsampled;
            m_ops->downsample(xform, m_dataRes, m_data, e.ires, m_volMax, inRange, outRange, downsampled);
            m_volume->activateRegionFromValues(e, downsampled.data(), 0.05f);
            if (m_halo) m_volume->activateHalo(e);
        }
        m_volume->finishTopology();
        m_volume->updateAtlas();
        m_ops->clearAllChannels();

        log::info("Resample.");
        m_ops->resample(0, xform, m_dataRes, m_data, inRange, outRange);
        m_volume->logMeasure();
    }

    void OnRender(uint32_t width, uint32_t height) override {
        getCamera()->verticalFov = verticalFovFromHorizontal(dm::radians(50.f), float(width) / float(height));
        updateScene();
        RenderView view = getRenderView(int(width), int(height));
        m_renderer->render(getSceneGraph(), view, getPresenter()->getRenderBuffer(0));
        if (m_showTopology) getDebugDraw()->topology(m_instance);
    }

    void OnBuildUI() override {
        ImGui::Checkbox("Topology", &m_showTopology);
        bool changed = false;
        changed |= ImGui::Checkbox("Halo", &m_halo);
        changed |= ImGui::Checkbox("Sparse", &m_sparse);
        if (changed) rebuild();
        gvdb::GVDBUsage usage = m_volume->getUsage();
        ImGui::Text("Bricks: %llu, %.1f MVoxels, atlas %.1f MB", (unsigned long long)usage.numBricks, usage.millionVoxels,
                    usage.atlasMB);
    }

    bool OnKey(int key, int scancode, int action, int mods) override {
        if (action != GLFW_PRESS) return false;
        switch (key) {
            case GLFW_KEY_1: m_showTopology = !m_showTopology; return true;
            case GLFW_KEY_2: m_sparse = !m_sparse; rebuild(); return true;
            case GLFW_KEY_3: m_halo = !m_halo; rebuild(); return true;
            default: return false;
        }
    }

 private:
    nvrhi::AutoPtr<gp::IBuffer> m_data;
    dm::int3 m_dataRes = dm::int3::zero();
    dm::float3 m_volMax = dm::float3::zero();
    nvrhi::AutoPtr<gvdb::IGVDBVolume> m_volume;
    nvrhi::AutoPtr<gvdb::IGVDBVoxelOps> m_ops;
    nvrhi::AutoPtr<gvdb::GVDBVolumeInstance> m_instance;
    std::unique_ptr<VolRenderer> m_renderer;
    bool m_showTopology = false;
    bool m_sparse = false;
    bool m_halo = true;
};

int main(int argc, const char **argv) {
    GVDBAppOptions options;
    options.title = "GVDB Voxels - resample";
    options.sampleName = GVDB_SAMPLE_NAME;
    return runGVDBApp<ResampleApp>(argc, argv, options);
}

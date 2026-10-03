// point-cloud (gPointCloud): a point cloud time series (ushort3 positions in
// pnt%04d.dat / points.dat) is converted on the GPU, its topology rebuilt
// (IGVDBPointOps::rebuildTopology), the points binned into brick sub-cells
// and gathered into a level set channel (FP16 paths), smoothed, and rendered
// progressively with the OptiX renderer next to the polygon models of a
// scene file (*.scn, parsed by SceneFile.cpp: materials, models, camera,
// light, volume parameters). The CUDA raycaster (VolRenderer) is used when
// --cuda is given or no OptiX device exists (the reference's m_render_optix
// = false path: no polygons, shading selectable).
//
// Command line: --in <file.scn> (default teapot.scn, looked up in the assets),
// --frame N (first frame), --scale S (render scale), --info (log the volume
// statistics after each update), --key (the reference's camera key frames for
// frames 1100..1340), --cuda. Unattended: GVDBApp's --screenshot <file.png>
// [--frames N] saves the back buffer after N presented frames; one presented
// frame is one OptiX sample, so --samples N is accepted as an alias of
// --frames N here (the accumulation is not restarted before the capture).
// Keys: 1 = draw points, 2 = draw topology. Mouse: GVDBApp orbit controls
// (left: orbit, middle: pan, right: distance, shift: light).
#include "SceneFile.h"
#include <sample-utils/GVDBApp.h>
#include <sample-utils/OptixRenderer.h>
#include <sample-utils/VolRenderer.h>
#include <sample-utils/ObjMesh.h>
#include <sample-utils/SampleTypes.h>
#include <sample-utils/ImageIO.h>   // loadPNG (environment map)
#include <gvdb/GVDB.h>
#include <donut/core/log.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace donut;
using namespace SampleUtils;
using namespace PointCloud;

namespace {

// Options from the command line (parsed in main, before the window exists,
// because the scene file sets the window size).
struct Options {
    SceneDesc scene;
    std::filesystem::path sceneFile;
    int frame = -1;           // --frame (overrides the scene file)
    bool keyAnimation = false;
    bool info = false;
    bool forceCuda = false;
    int screenshotSamples = 0;   // --samples N: OptiX samples before GVDBApp's screenshot capture (0: --frames)
};

dm::float3 interp(dm::float3 a, dm::float3 b, float t) { return a + (b - a) * t; }

}  // namespace

class PointCloudApp : public GVDBApp {
    NVRHI_INHERIT_INTERFACE_TABLE()
 public:
    PointCloudApp(app::DeviceManager *dm, const Options &options) : GVDBApp(dm), m_opt(options) {}

    // ---- files ----------------------------------------------------------

    // Reference FindFile: a path prefix from the scene file is used as given,
    // otherwise the file is looked up in the assets.
    std::filesystem::path resolve(const std::string &prefix, const std::string &name) const {
        if (!prefix.empty()) return std::filesystem::path(prefix + name);
        std::filesystem::path p(name);
        if (p.is_absolute() && std::filesystem::exists(p)) return p;
        return getAssetPath(name.c_str());
    }

    // load_points: header (int count, float3 wMin, float3 wMax), then count
    // ushort3 positions; converted to float3 index space on the GPU
    // (ConvertAndTransform with the render scale) and set as the point cloud.
    bool loadPoints(int frame) {
        const SceneDesc &s = m_opt.scene;
        std::filesystem::path path = resolve(s.pointPath, formatFrameName(s.pointFile, frame));
        log::info("Load points from %s...", path.string().c_str());

        std::ifstream file(path, std::ios::binary);
        if (!file) {
            log::error("Cannot open file: %s", path.string().c_str());
            return false;
        }
        int32_t count = 0;
        dm::float3 wMin, wMax;
        file.read(reinterpret_cast<char *>(&count), sizeof(count));
        file.read(reinterpret_cast<char *>(&wMin), sizeof(wMin));
        file.read(reinterpret_cast<char *>(&wMax), sizeof(wMax));
        if (!file || count <= 0) {
            log::error("Bad point file header: %s", path.string().c_str());
            return false;
        }
        std::vector<uint16_t> packed(size_t(count) * 3);
        file.read(reinterpret_cast<char *>(packed.data()), std::streamsize(packed.size() * sizeof(uint16_t)));
        if (!file) {
            log::error("Truncated point file: %s", path.string().c_str());
            return false;
        }

        // GPU buffers (grown when a frame has more points)
        if (!m_pnt1 || m_pnt1->getDesc()->byteSize < packed.size() * sizeof(uint16_t)) {
            gp::BufferDesc desc;
            desc.byteSize = packed.size() * sizeof(uint16_t);
            UT_V_GP(getGPDevice()->createBuffer(desc, &m_pnt1));
            desc.byteSize = size_t(count) * sizeof(dm::float3);
            UT_V_GP(getGPDevice()->createBuffer(desc, &m_pnts));
            desc.isStaging = true;
            UT_V_GP(getGPDevice()->createBuffer(desc, &m_pntsStaging));
        }
        UT_V_GP(getGPQueue()->writeBuffer(m_pnt1, packed.data(), packed.size() * sizeof(uint16_t), 0));
        m_numPnts = uint32_t(count);

        // convert format and transform: ushort (2) -> float (4), (wMin + p * wDelta) * renderScale
        gvdb::PointData src, dst, none;
        src.buffer = m_pnt1;
        src.stride = 3 * sizeof(uint16_t);
        src.count = m_numPnts;
        dst.buffer = m_pnts;
        dst.stride = sizeof(dm::float3);
        dst.count = m_numPnts;
        const dm::float3 wDelta = (wMax - wMin) / 65535.f;
        m_points->convertAndTransform(src, 2, dst, 4, m_numPnts, wMin, wDelta, dm::float3::zero(),
                                      dm::float3(m_renderScale));
        m_points->setPoints(dst, none, none);

        // host copy for drawing the points (the reference's RetrieveData)
        UT_V_GP(getGPQueue()->copyBufferRegion(m_pntsStaging, 0, m_pnts, 0, size_t(m_numPnts) * sizeof(dm::float3)));
        syncQueue(getGPDevice(), getGPQueue());
        void *mapped = nullptr;
        UT_V_GP(getGPDevice()->mapBuffer(m_pntsStaging, &mapped));
        m_pntsHost.resize(m_numPnts);
        if (mapped) memcpy(m_pntsHost.data(), mapped, size_t(m_numPnts) * sizeof(dm::float3));
        getGPDevice()->unmapBuffer(m_pntsStaging);

        log::info("  Done. %u points, bounds <%.1f, %.1f, %.1f> - <%.1f, %.1f, %.1f>", m_numPnts, wMin.x, wMin.y,
                  wMin.z, wMax.x, wMax.y, wMax.z);
        return true;
    }

    // load_polys: the polygon time series is one mesh whose buffers are
    // replaced per frame.
    bool loadPolys(int frame) {
        const SceneDesc &s = m_opt.scene;
        std::filesystem::path path = resolve(s.polyPath, formatFrameName(s.polyFile, frame));
        log::info("Load polydata from %s...", path.string().c_str());
        nvrhi::AutoPtr<engine::MeshInfo> mesh = loadObjMesh(nullptr, path, s.polyScale, s.polyOffset);
        if (!mesh) return false;
        if (!m_polyMesh) {
            m_polyMesh = mesh;
            auto instance = MAKE_RC_OBJ_PTR(engine::MeshInstance, m_polyMesh);
            engine::SceneGraphNode *node = getSceneGraph()->AttachLeafNode(getSceneGraph()->GetRootNode(), instance);
            node->SetName("polys");
            setNodeTransform(node, dm::scaling(dm::float3(m_renderScale)));
            m_meshInstances.push_back(instance);
            m_meshMaterials.push_back(s.polyMaterial);
        } else {
            m_polyMesh->buffers = mesh->buffers;
            m_polyMesh->geometries = mesh->geometries;
            m_polyMesh->objectSpaceBounds = mesh->objectSpaceBounds;
            m_polyMesh->totalIndices = mesh->totalIndices;
            m_polyMesh->totalVertices = mesh->totalVertices;
        }
        return true;
    }

    // ---- initialisation --------------------------------------------------

    bool OnInit() override {
        const SceneDesc &s = m_opt.scene;
        m_renderScale = s.renderScale;
        m_frame = m_opt.frame >= 0 ? m_opt.frame : s.frame;
        m_maxSamples = std::max(1, s.maxSamples);
        m_smooth = s.smooth;
        m_smoothParams = s.smoothParams;
        m_useOptix = !m_opt.forceCuda && getRTDevice() != nullptr;
        if (!m_useOptix)
            log::warning("point-cloud: rendering with the CUDA raycaster (%s).",
                         m_opt.forceCuda ? "--cuda" : "no OptiX device");
        if (isScreenshotRun()) {
            // --samples N: capture after N accumulated samples; keep the frame
            // from advancing (which would restart the accumulation) before that.
            if (m_opt.screenshotSamples > 0) m_screenshotFrames = uint32_t(m_opt.screenshotSamples);
            m_maxSamples = std::max(m_maxSamples, int(m_screenshotFrames) + 1);
        }

        // camera / light (defaults of the reference; the scene file scales target and distance)
        getCamera()->zNear = 1.f;
        getCamera()->zFar = 10000.f;
        getCameraOrbit().setOrbit(s.cameraAngles, s.cameraTarget * m_renderScale, s.cameraDistance * m_renderScale);
        getLightOrbit().setOrbit(s.lightAngles, s.lightTarget * m_renderScale, s.lightDistance * m_renderScale);

        // ---- volume: Configure(3,3,3,3,4), SetChannelDefault(32,32,1), one float channel ----
        m_volume = createVolume();
        if (!m_volume) return false;
        m_volume->configure(gvdb::GVDBLevelConfig::fromBranching(3, 3, 3, 3, 4));
        gvdb::GVDBAtlasConfig atlas;
        atlas.gridDim = {32u, 32u, 1u};
        m_volume->configureAtlas(atlas);
        gvdb::GVDBChannelDesc levelSet;
        levelSet.format = gvdb::ATLAS_FORMAT_R32_FLOAT;
        m_volume->addChannel(levelSet);
        if (NVRHI_FAILED(gvdb::createGVDBVoxelOps(m_volume, &m_ops))) return false;
        if (NVRHI_FAILED(gvdb::createGVDBPointOps(m_volume, &m_points))) return false;

        // The points are scaled into index space when they are converted, so
        // the volume's index space is the world (the reference's identity
        // getTransform()); the polygons are scaled by the render scale instead.
        m_instance = addVolumeInstance(m_volume, dm::affine3::identity(), "points");
        gvdb::VolumeRenderAttributes &attrs = m_instance->GetRenderAttributes();
        attrs.channel = 0;
        attrs.shading = gvdb::VolumeShading::LevelSet;
        attrs.steps = s.steps;
        attrs.extinct = s.extinct;
        attrs.threshold = s.range;
        attrs.cutoff = s.cutoff;
        attrs.materialId = s.pointMaterial;
        // the only transfer function the reference defines (handle_gui): blue -> green -> red
        attrs.transferFunction = MAKE_RC_OBJ_PTR(gvdb::VolumeTransferFunction, getGPDevice());
        attrs.transferFunction->setLinear(0.00f, 0.50f, {0.f, 0.f, 1.f, 0.f}, {0.f, 1.f, 0.f, 0.1f});
        attrs.transferFunction->setLinear(0.50f, 1.00f, {0.f, 1.f, 0.f, 0.1f}, {1.f, 0.f, 0.f, 0.1f});
        attrs.transferFunction->commit(getGPQueue());

        // ---- polygon models (Scene::AddModel(file, scale, offset), instanced at the render scale) ----
        for (const SceneModel &model : s.models) {
            std::filesystem::path path = resolve(model.path, model.file);
            log::info("Load model %s...", path.string().c_str());
            nvrhi::AutoPtr<engine::MeshInfo> mesh = loadObjMesh(nullptr, path, model.scale, model.offset);
            if (!mesh) return false;
            auto instance = MAKE_RC_OBJ_PTR(engine::MeshInstance, mesh);
            engine::SceneGraphNode *node = getSceneGraph()->AttachLeafNode(getSceneGraph()->GetRootNode(), instance);
            node->SetName(model.file);
            setNodeTransform(node, dm::scaling(dm::float3(m_renderScale)));
            m_meshes.push_back(mesh);
            m_meshInstances.push_back(instance);
            m_meshMaterials.push_back(model.material);
        }

        // ---- input data ----
        if (s.pointsOn && !loadPoints(m_frame)) return false;
        if (s.polysOn && !loadPolys(m_polyFrame = s.polyFrame)) return false;
        renderUpdate();
        updateScene();

        // ---- renderers ----
        if (m_useOptix) {
            if (s.materials.empty()) {
                log::error("No materials have been specified in scene.");
                return false;
            }
            m_optix = std::make_unique<OptixRenderer>(getGPDevice(), getRTDevice(), getGPQueue(), getVFS());
            m_optix->getViewParams().backgroundColor = s.backgroundColor;
            for (const OptixMaterialParams &m : s.materials) m_optix->addMaterial(m);
            for (size_t i = 0; i < m_meshInstances.size(); ++i)
                m_optix->setMeshMaterial(m_meshInstances[i], m_meshMaterials[i]);
            if (!s.envmap.empty()) {
                std::vector<uint8_t> rgba;
                uint32_t w = 0, h = 0;
                std::filesystem::path env = resolve("", s.envmap);
                if (loadPNG(env, rgba, w, h)) {
                    log::info("Loading env map %s.", env.string().c_str());
                    m_optix->setEnvmap(rgba.data(), w, h);
                } else {
                    log::warning("Cannot load env map %s", env.string().c_str());
                }
            }
            log::info("Building the OptiX scene.");
            m_optix->buildScene(getSceneGraph());
        } else {
            m_cuda = std::make_unique<VolRenderer>(getGPDevice(), getGPQueue(), getVFS());
            m_cuda->getViewParams().backgroundColor = s.backgroundColor;
        }
        return true;
    }

    // render_update: points -> topology -> level set.
    void renderUpdate() {
        if (!m_opt.scene.pointsOn || m_numPnts == 0) return;
        const float radius = float(m_radius);
        const int subcellSize = 4;

        log::info("Dynamic topology.");
        m_points->rebuildTopology(m_numPnts, radius * 2.f, m_origin);
        m_volume->finishTopology(true);
        m_volume->updateAtlas();

        log::info("Points to voxels.");
        m_ops->clearChannel(0);
        int scPntLen = 0;
        m_points->insertPointsSubcellFP16(subcellSize, m_numPnts, radius, m_origin, scPntLen);
        m_points->gatherLevelSetFP16(subcellSize, m_numPnts, radius, m_origin, scPntLen, 0, gvdb::CHAN_UNDEF);
        m_ops->updateApron(0, 3.f);

        if (m_smooth > 0) {
            log::info("Smooth: %d, %f %f %f", m_smooth, m_smoothParams.x, m_smoothParams.y, m_smoothParams.z);
            m_ops->compute(gvdb::ComputeOp::Smooth, 0, m_smooth, m_smoothParams, true, true, 3.f);
        }
        if (m_optix) m_optix->updateVolumes(getSceneGraph());
        if (m_opt.info) m_volume->logMeasure();
        m_sample = 0;
    }

    // The end of a frame: advance the time series (the reference also saved a PNG here).
    void nextFrame() {
        const SceneDesc &s = m_opt.scene;
        m_frame += s.frameStep;
        // NOTE(port): the reference reloads and rebuilds even when the frame did
        // not change (fstep 0, as in teapot.scn); skipped here.
        bool changed = false;
        if (s.pointsOn && s.frameStep != 0 && loadPoints(m_frame)) changed = true;
        if (s.polysOn && s.polyFrameStep != 0) {
            m_polyFrame += s.polyFrameStep;
            if (loadPolys(m_polyFrame) && m_optix) m_optix->updateMeshes(getSceneGraph());
        }
        if (changed) renderUpdate();
    }

    // ---- frame ----------------------------------------------------------

    void OnRender(uint32_t width, uint32_t height) override {
        getCamera()->verticalFov = verticalFovFromHorizontal(dm::radians(m_opt.scene.cameraFov), float(width) / float(height));

        if (m_opt.keyAnimation && m_frame >= 1100 && m_frame <= 1340) {
            float u = float(m_frame - 1100) / float(1340 - 1100);
            dm::float3 a0 = interp({0.f, 35.f, 0.f}, {0.f, 24.6f, 0.f}, u);
            dm::float3 t0 = interp({240.f, 0.f, 0.f}, {362.f, -201.f, 20.f}, u);
            dm::float3 d0 = interp({1600.f, 0.f, 0.f}, {2132.f, 0.f, 0.f}, u);
            getCameraOrbit().setOrbit(a0, t0 * m_renderScale, d0.x * m_renderScale);
        }
        updateScene();
        RenderView view = getRenderView(int(width), int(height));

        if (m_optix) {
            if (m_optix->getWidth() != width || m_optix->getHeight() != height) {
                m_optix->resizeOutput(width, height);
                m_sample = 0;
            }
            m_optix->setSample(m_frame, m_sample);
            m_optix->render(getSceneGraph(), view, gvdb::VolumeShading::LevelSet);
            UT_V_GP(getGPQueue()->copyBufferRegion(getPresenter()->getRenderBuffer(0), 0, m_optix->getOutputBuffer(), 0,
                                                   size_t(width) * height * 4));
        } else {
            static const gvdb::VolumeShading shadings[] = {gvdb::VolumeShading::Off,       gvdb::VolumeShading::Voxel,
                                                           gvdb::VolumeShading::EmptySkip, gvdb::VolumeShading::Section3D,
                                                           gvdb::VolumeShading::Volume,    gvdb::VolumeShading::LevelSet};
            m_instance->GetRenderAttributes().shading = shadings[dm::clamp(m_shadeStyle, 0, 5)];
            m_cuda->getViewParams().sectionPoint = m_origin;
            m_cuda->getViewParams().sectionNormal = {0.f, 0.f, -1.f};
            m_cuda->render(getSceneGraph(), view, getPresenter()->getRenderBuffer(0));
        }

        if (m_sample % 8 == 0 && m_sample > 0 && m_optix) log::info("%d%%", (m_sample * 100) / m_maxSamples);
        if (++m_sample >= m_maxSamples) {
            m_sample = 0;
            nextFrame();
        }

        if (m_showPoints) drawPoints();
        if (m_showTopology) getDebugDraw()->topology(m_instance);
    }

    // draw_points: a short line per point, coloured by position / 256.
    // NOTE(port): at most c_maxDrawnPoints are drawn (every n-th point).
    void drawPoints() {
        static const size_t c_maxDrawnPoints = 250000;
        const size_t step = std::max<size_t>(1, (m_pntsHost.size() + c_maxDrawnPoints - 1) / c_maxDrawnPoints);
        const dm::affine3 toWorld = m_instance->GetIndexToWorld();
        for (size_t i = 0; i < m_pntsHost.size(); i += step) {
            dm::float3 p = toWorld.transformPoint(m_pntsHost[i]);
            dm::float3 c = p / 256.f;
            getDebugDraw()->line(p, p + dm::float3(0.5f), dm::float4(c.x, c.y, c.z, 1.f));
        }
    }

    void OnMouseDrag(int button, int dx, int dy, int mods) override {
        GVDBApp::OnMouseDrag(button, dx, dy, mods);
        m_sample = 0;   // camera or light moved: restart the accumulation
    }

    void OnBuildUI() override {
        ImGui::Checkbox("Points", &m_showPoints);
        ImGui::SameLine();
        ImGui::Checkbox("Topology", &m_showTopology);
        if (!m_optix) ImGui::Combo("Shading", &m_shadeStyle, "Off\0Voxel\0Empty skip\0Section\0Volume\0Level set\0\0");
        ImGui::Text("Frame %d, sample %d / %d, %u points", m_frame, m_sample, m_maxSamples, m_numPnts);
        gvdb::GVDBUsage usage = m_volume->getUsage();
        ImGui::Text("Bricks: %llu, %.1f MVoxels, atlas %.1f MB", (unsigned long long)usage.numBricks, usage.millionVoxels,
                    usage.atlasMB);
        ImGui::Text("Left: orbit, Middle: pan, Right: distance, Shift: light");
    }

    bool OnKey(int key, int scancode, int action, int mods) override {
        if (action != GLFW_PRESS) return false;
        switch (key) {
            case GLFW_KEY_1: m_showPoints = !m_showPoints; return true;
            case GLFW_KEY_2: m_showTopology = !m_showTopology; return true;
            default: return false;
        }
    }

 private:
    Options m_opt;
    float m_renderScale = 1.f;

    // points
    uint32_t m_numPnts = 0;
    nvrhi::AutoPtr<gp::IBuffer> m_pnt1;          // ushort3 as read from the file
    nvrhi::AutoPtr<gp::IBuffer> m_pnts;          // float3 index space
    nvrhi::AutoPtr<gp::IBuffer> m_pntsStaging;
    std::vector<dm::float3> m_pntsHost;
    int m_radius = 1;
    dm::float3 m_origin = dm::float3::zero();
    int m_smooth = 0;
    dm::float3 m_smoothParams = dm::float3::zero();

    // volume
    nvrhi::AutoPtr<gvdb::IGVDBVolume> m_volume;
    nvrhi::AutoPtr<gvdb::IGVDBVoxelOps> m_ops;
    nvrhi::AutoPtr<gvdb::IGVDBPointOps> m_points;
    nvrhi::AutoPtr<gvdb::GVDBVolumeInstance> m_instance;

    // polygons
    std::vector<nvrhi::AutoPtr<engine::MeshInfo>> m_meshes;
    nvrhi::AutoPtr<engine::MeshInfo> m_polyMesh;   // the polygon time series
    std::vector<nvrhi::AutoPtr<engine::MeshInstance>> m_meshInstances;
    std::vector<int> m_meshMaterials;
    int m_polyFrame = 0;

    // rendering
    bool m_useOptix = true;
    std::unique_ptr<OptixRenderer> m_optix;
    std::unique_ptr<VolRenderer> m_cuda;
    int m_frame = 0;
    int m_sample = 0;
    int m_maxSamples = 1;
    int m_shadeStyle = 5;   // CUDA path: level set
    bool m_showPoints = false;
    bool m_showTopology = false;
};

int main(int argc, const char **argv) {
    Options opt;
    std::string sceneName = "teapot.scn";
    for (int i = 1; i < argc; ++i) {
        auto is = [&](const char *a, const char *b) { return strcmp(argv[i], a) == 0 || strcmp(argv[i], b) == 0; };
        if (is("--in", "-in") && i + 1 < argc) sceneName = argv[++i];
        else if (is("--frame", "-frame") && i + 1 < argc) opt.frame = atoi(argv[++i]);
        else if (is("--scale", "-scale") && i + 1 < argc) opt.scene.renderScale = float(atof(argv[++i]));
        else if (is("--key", "-key")) opt.keyAnimation = true;
        else if (is("--info", "-info")) opt.info = true;
        else if (is("--cuda", "-cuda")) opt.forceCuda = true;
        else if (strcmp(argv[i], "--samples") == 0 && i + 1 < argc) opt.screenshotSamples = atoi(argv[++i]);
        // --screenshot <png> / --frames N are handled by runGVDBApp
    }

    // the scene file: as given, or from the assets
    opt.sceneFile = sceneName;
    if (!std::filesystem::exists(opt.sceneFile)) opt.sceneFile = std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR) / sceneName;
    log::info("input: %s", opt.sceneFile.string().c_str());
    if (!parseSceneFile(opt.sceneFile, opt.scene)) return 1;
    if (opt.scene.renderScale == 0.f) opt.scene.renderScale = 1.f;
    if (opt.scene.frame < 0) opt.scene.frame = 0;

    GVDBAppOptions options;
    options.title = "GVDB Voxels - point-cloud";
    options.sampleName = GVDB_SAMPLE_NAME;
    options.width = uint32_t(std::max(64, opt.scene.width));
    options.height = uint32_t(std::max(64, opt.scene.height));
    options.wantOptix = !opt.forceCuda;
    return runGVDBApp(argc, argv, options, [&](app::DeviceManager *dm) -> nvrhi::AutoPtr<GVDBApp> {
        return nvrhi::TakeOver(MAKE_RC_OBJ(PointCloudApp, dm, opt));
    });
}

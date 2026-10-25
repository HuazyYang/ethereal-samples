// point-fusion (gPointFusion): a procedural city of boxes (buildings, curbs,
// streets) is scanned by a simulated depth camera mounted on a car
// (PointFusionScan.slang, shaders/gvdb/PointFusionScan.nvsc): every frame one
// 240x180 jittered depth image is turned into points, the points are fused
// incrementally into one volume (accumulateTopology -> updateAtlas ->
// insertPointsSubcell -> gatherLevelSet(accumulate) -> updateApron) and the
// level set is rendered with the CUDA raycaster. The scanner is a second
// PerspectiveCamera leaf of the scene graph; the city boxes, the scan points
// and the topology can be drawn as lines.
//
// Controls (reference mapping): left drag = orbit camera (or scanner angles in
// FPV / with shift), middle = pan (scanner height in FPV / shift), right =
// orbit distance (car speed in FPV / shift); keys: 1 points, 2 topology,
// space = scan on/off. Command line: --speed <v> initial car speed, --orbit
// starts with the orbit camera instead of the first-person view, --color
// enables the colour channel; GVDBApp's --screenshot <png> [--frames N] saves
// the frame after N scans.
#include <sample-utils/GVDBApp.h>
#include <sample-utils/SampleTypes.h>
#include <gvdb/GVDB.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <nvrhi/core/datablob.h>
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace donut;
using namespace SampleUtils;

namespace {

constexpr int GRID_X = 10;
constexpr int GRID_Y = 10;
constexpr int GRID_CNT = GRID_X * GRID_Y;
constexpr float GRID_SCALE = 5.0f;   // voxels per metre
constexpr int SEED_COUNT = 128 * 128;

// Same layout as ScanObj / ScanInfo in PointFusionScan.slang (an array of 48-byte objects; the scan constants
// by value as a kernel argument).
struct GVDB_ALIGN(16) ScanObj {
    dm::float3 pos;
    dm::float3 size;
    dm::float3 loc;
    uint32_t clr;
};
static_assert(sizeof(ScanObj) == 48, "ScanObj layout differs from PointFusionScan.slang");
struct ScanInfo {
    dm::int3 gridRes;
    dm::float3 gridSize;
    dm::float3 cams;
    dm::float3 camu;
    dm::float3 camv;
};

// Direction from the orbit target to the eye (the reference Camera3D angles).
dm::float3 orbitDirection(dm::float3 anglesDeg) {
    float ax = dm::radians(anglesDeg.x), ay = dm::radians(anglesDeg.y);
    return dm::float3(cosf(ay) * sinf(ax), sinf(ay), cosf(ay) * cosf(ax));
}

// Camera3D::setPos + setAngles: eye at `position`, looking along `anglesDeg`,
// the target `distance` ahead.
void placeOrbit(OrbitController &orbit, dm::float3 position, dm::float3 anglesDeg) {
    orbit.angles = anglesDeg;
    orbit.target = position - orbitDirection(anglesDeg) * orbit.distance;
}

dm::float4 unpackColorA(uint32_t c) {
    return dm::float4(float(c & 255) / 255.f, float((c >> 8) & 255) / 255.f, float((c >> 16) & 255) / 255.f,
                      float((c >> 24) & 255) / 255.f);
}

}  // namespace

class PointFusionApp : public GVDBApp {
    NVRHI_INHERIT_INTERFACE_TABLE()
 public:
    using GVDBApp::GVDBApp;

    void setInitialSpeed(float speed) { m_speed = speed; }
    void setFirstPersonView(bool pov) { m_showPov = pov; }
    void setUseColor(bool color) { m_useColor = color; }

    bool OnInit() override {
        // ---- volume (SetupGVDB of the reference) ----
        m_volume = createVolume();
        if (!m_volume) return false;
        m_volume->configure(gvdb::GVDBLevelConfig::fromBranching(3, 3, 3, 3, 5));
        gvdb::GVDBAtlasConfig atlas;
        atlas.gridDim = {16u, 16u, 8u};   // SetChannelDefault(16, 16, 8)
        m_volume->configureAtlas(atlas);
        if (NVRHI_FAILED(gvdb::createGVDBVoxelOps(m_volume, &m_ops))) return false;
        if (NVRHI_FAILED(gvdb::createGVDBPointOps(m_volume, &m_points))) return false;

        m_instance = addVolumeInstance(m_volume, dm::affine3::identity(), "fusion");
        gvdb::VolumeRenderAttributes &attrs = m_instance->GetRenderAttributes();
        attrs.channel = 0;
        attrs.shading = gvdb::VolumeShading::LevelSet;   // m_shade_style = 5
        attrs.steps = {0.5f, 16.f, 0.5f};
        attrs.extinct = {-1.0f, 1.1f, 0.0f};
        attrs.threshold = {0.0f, 3.0f, -1.0f};           // SetVolumeRange(0, 3, -1)
        attrs.cutoff = {0.005f, 0.001f, 0.0f};
        attrs.transferFunction = MAKE_RC_OBJ_PTR(gvdb::VolumeTransferFunction, getCompute());
        attrs.transferFunction->setLinear(0.f, 1.f, {0.f, 0.f, 0.f, 0.f}, {1.f, 1.f, 1.f, 0.1f});
        attrs.transferFunction->commit();

        m_renderer = std::make_unique<VolRenderer>(getCompute());
        m_renderer->getViewParams().backgroundColor = {0.1f, 0.2f, 0.3f, 1.f};
        m_renderer->getViewParams().shadowParams = {0.8f, 1.f, 0.f};

        // ---- scan kernel ----
        m_scanKernel = getCompute()->getKernel("shaders/gvdb/PointFusionScan.nvsc", "scanBuildings");
        if (!m_scanKernel) return false;

        // ---- point lists, hit counter, jitter seeds ----
        nvrhi::IDevice *dev = getCudaDevice();
        auto makeBuffer = [dev](size_t bytes, bool readback) {
            nvrhi::BufferDesc desc;
            desc.byteSize = bytes;
            if (readback) desc.cpuAccess = nvrhi::CpuAccessMode::Read;
            nvrhi::BufferHandle b;
            UT_CHECK(dev->createBuffer(desc, &b));
            return b;
        };
        m_pntsGP = makeBuffer(size_t(m_maxPnts) * sizeof(dm::float3), false);
        m_clrsGP = makeBuffer(size_t(m_maxPnts) * sizeof(uint32_t), false);
        m_pntsStaging = makeBuffer(size_t(m_maxPnts) * sizeof(dm::float3), true);
        m_clrsStaging = makeBuffer(size_t(m_maxPnts) * sizeof(uint32_t), true);
        m_pntOutGP = makeBuffer(sizeof(int), false);
        m_pntOutStaging = makeBuffer(sizeof(int), true);
        m_seedsGP = makeBuffer(size_t(SEED_COUNT) * sizeof(uint32_t), false);
        {
            // The reference seeded with rand()/RAND_MAX (integer division: all
            // zero); proper seeds give every pixel its own jitter sequence.
            std::vector<uint32_t> seeds(SEED_COUNT);
            srand(1043);
            for (auto &s : seeds) s = uint32_t(rand());
            getCompute()->getCommandList()->writeBuffer(m_seedsGP, seeds.data(), seeds.size() * sizeof(uint32_t), 0);
        }
        // Brick-sized blocks of the "far" level set value (3.0, FillChannel(0, 3)
        // of the reference) and of transparent black for the colour channel,
        // copied into every newly activated brick (see fillNewBricks).
        {
            dm::uint3 bwa = m_volume->getBrickDimWithApron();
            size_t voxels = size_t(bwa.x) * bwa.y * bwa.z;
            std::vector<float> farValues(voxels, 3.0f);
            m_fillFar = makeBuffer(voxels * sizeof(float), false);
            getCompute()->getCommandList()->writeBuffer(m_fillFar, farValues.data(), voxels * sizeof(float), 0);
            m_fillZero = makeBuffer(voxels * sizeof(uint32_t), false);
            getCompute()->getCommandList()->clearBufferUInt(m_fillZero, 0u);
        }

        setupChannels();
        generateCity();

        // ---- cameras and light ----
        getCamera()->zNear = 1.f;
        getCamera()->zFar = 100000.f;
        getCameraOrbit().setOrbit({190.f, 30.f, 0.f}, dm::float3::zero(), 4200.f);

        // The scanner: a camera leaf of its own, driven like the reference m_carcam
        // (fov 90, dist 1400, at (2 blocks, 4 m, 2 blocks) looking along +z).
        m_scanCamera = MAKE_RC_OBJ_PTR(engine::PerspectiveCamera);
        m_scanCamera->zNear = 1.f;
        m_scanCamera->zFar = 100000.f;
        m_scanCamera->verticalFov = verticalFovFromHorizontal(dm::radians(90.f), float(m_scanRes.x) / float(m_scanRes.y));
        m_scanCameraNode = getSceneGraph()->AttachLeafNode(getSceneGraph()->GetRootNode(), m_scanCamera);
        m_scanCameraNode->SetName("scanner");
        m_car.distance = 1400.f;
        placeOrbit(m_car, dm::float3(2.f * m_gridSize * GRID_SCALE, 4.f * GRID_SCALE, 2.f * m_gridSize * GRID_SCALE),
                   dm::float3(180.f, 0.f, 0.f));
        m_car.apply(m_scanCameraNode);

        getLightOrbit().setOrbit({42.f, 40.f, 0.f}, m_car.getPosition(), 2000.f * GRID_SCALE);
        if (!m_showPov) {   // --orbit: as if the first-person view had just been left (SwitchCamera)
            getCameraOrbit().setOrbit({190.f, 30.f, 0.f}, m_car.getPosition(), getCameraOrbit().distance);
        }
        updateScene();
        return true;
    }

    // Channels of the fused volume (SetupGVDB): level set, plus a colour channel
    // when enabled. Rebuilds the topology from scratch with the next scan.
    void setupChannels() {
        m_volume->clear();
        m_volume->destroyChannels();
        gvdb::GVDBChannelDesc levelSet;
        levelSet.format = gvdb::ATLAS_FORMAT_R32_FLOAT;
        m_volume->addChannel(levelSet);
        if (m_useColor) {
            gvdb::GVDBChannelDesc color;
            color.format = gvdb::ATLAS_FORMAT_RGBA8_UINT;
            color.samplerDesc.minFilter = false;   // F_POINT
            color.samplerDesc.magFilter = false;
            color.samplerDesc.mipFilter = false;
            m_volume->addChannel(color);
        }
        m_instance->GetRenderAttributes().colorChannel = m_useColor ? 1 : gvdb::CHAN_UNDEF;

        gvdb::PointData pos, vel, clr;
        pos.buffer = m_pntsGP;
        pos.stride = sizeof(dm::float3);
        pos.count = uint32_t(m_maxPnts);
        if (m_useColor) {
            clr.buffer = m_clrsGP;
            clr.stride = sizeof(uint32_t);
            clr.count = uint32_t(m_maxPnts);
        }
        m_points->setPoints(pos, vel, clr);
        m_points->requestFullRebuild(true);
        m_filledBricks.clear();
        m_totalPnts = 0;
    }

    // ---- the city (GenerateCity of the reference) ----

    dm::float3 getBuildingPos(dm::float3 bloc, float blockMax, dm::float3 bdim, dm::float3 &bsz) const {
        dm::float3 p = dm::float3::zero();
        switch (int(bloc.y)) {   // place building along edge of city block
            case 0: p = {bloc.x * blockMax, 0.f, 0.f}; bsz = {bdim.x, bdim.z, bdim.y}; break;
            case 1: p = {blockMax - bdim.y, 0.f, bloc.x * blockMax}; bsz = {bdim.y, bdim.z, bdim.x}; break;
            case 2: p = {(1.f - bloc.x) * blockMax - bdim.x, 0.f, blockMax - bdim.y}; bsz = {bdim.x, bdim.z, bdim.y}; break;
            case 3: p = {0.f, 0.f, (1.f - bloc.x) * blockMax - bdim.x}; bsz = {bdim.y, bdim.z, bdim.x}; break;
            default: break;
        }
        return p;
    }

    void generateBuilding(dm::float3 &bloc, dm::float3 &bpos, dm::float3 &bsz, float blockMax, float distCtr) {
        float hgt = 3.0f + 200.f * expf(distCtr / -200.0f);
        dm::float3 bdim(random(5.f, 20.f), random(10.f, 40.f), random(3.f, hgt));   // x = along street, y = away from street
        if (bloc.x + bdim.x / blockMax > 1.0f) bdim.x = (1.f - bloc.x) * blockMax;

        bpos = getBuildingPos(bloc, blockMax, bdim, bsz);

        // next building location
        bloc.x += bdim.x / blockMax;
        if (bloc.x >= 1.f) {
            bloc.x = bdim.y / blockMax;
            bloc.y++;
        }
    }

    float random(float lo, float hi) { return lo + (hi - lo) * std::uniform_real_distribution<float>(0.f, 1.f)(m_rng); }

    void generateCity() {
        const float blockSz = 100.f;   // size of city block in metres
        const float laneSz = 7.4f;     // width of street (metres)
        const float curbSz = 3.7f;     // width of curb
        m_gridSize = blockSz + laneSz * 2.f + curbSz * 2.f;
        m_cityCenter = {GRID_X * m_gridSize * 0.5f, 0.f, GRID_Y * m_gridSize * 0.5f};

        m_objs.clear();
        m_objs.reserve(GRID_CNT * 100);
        for (int y = 0; y < GRID_Y; ++y) {
            for (int x = 0; x < GRID_X; ++x) {
                dm::float3 blockPos(x * m_gridSize + laneSz + curbSz, 0.f, y * m_gridSize + laneSz + curbSz);
                dm::float3 bloc = dm::float3::zero(), bpos, bsize, unused;
                int bcnt = 0;
                while (bloc.y < 4.f && bcnt < 100) {   // generate buildings in block
                    float distCtr = dm::length(m_cityCenter - (blockPos + getBuildingPos(bloc, blockSz, dm::float3::zero(), unused)));
                    generateBuilding(bloc, bpos, bsize, blockSz, distCtr);
                    ScanObj bldg;
                    bldg.clr = packColorA(random(0.f, 0.9f), random(0.f, 0.5f), 0.f, 1.f);
                    bldg.loc = bloc;
                    bldg.pos = (blockPos + bpos) * GRID_SCALE;
                    bldg.size = bsize * GRID_SCALE;
                    m_objs.push_back(bldg);
                    ++bcnt;
                }
                ScanObj curb;   // generate curb
                curb.clr = packColorA(.3f, .3f, .3f, 1.f);
                curb.loc = dm::float3::zero();
                curb.pos = (blockPos - dm::float3(curbSz, 0.f, curbSz)) * GRID_SCALE;
                curb.size = dm::float3(blockSz + curbSz * 2.f, 0.25f, blockSz + curbSz * 2.f) * GRID_SCALE;
                m_objs.push_back(curb);
                ScanObj street;   // generate street
                street.clr = packColorA(.5f, .5f, .5f, 1.f);
                street.loc = dm::float3::zero();
                street.pos = (blockPos - dm::float3(laneSz + curbSz, 0.f, laneSz + curbSz)) * GRID_SCALE;
                street.size = dm::float3(blockSz + (laneSz + curbSz) * 2.f, 0.01f, blockSz + (laneSz + curbSz) * 2.f) * GRID_SCALE;
                m_objs.push_back(street);
            }
        }

        nvrhi::BufferDesc desc;
        desc.byteSize = m_objs.size() * sizeof(ScanObj);
        UT_CHECK(getCudaDevice()->createBuffer(desc, &m_objsGP));
        getCompute()->getCommandList()->writeBuffer(m_objsGP, m_objs.data(), desc.byteSize, 0);
        log::info("City: %d objects.", int(m_objs.size()));
    }

    // ---- scanning (ScanBuildings) ----

    void scanBuildings() {
        RenderView scanView = makeRenderView(m_scanCamera, getLight(), m_scanRes.x, m_scanRes.y);

        ScanInfo info = {};
        info.gridRes = {GRID_X, GRID_Y, 0};
        info.gridSize = dm::float3(GRID_X * m_gridSize, GRID_Y * m_gridSize, 0.f) * GRID_SCALE;
        info.cams = scanView.rayTopLeft;
        info.camu = scanView.rayU;
        info.camv = scanView.rayV;

        m_numPnts = m_scanRes.x * m_scanRes.y;   // max number of points per frame
        const float tmax = m_gridSize * 4.f * GRID_SCALE;
        const dm::float3 pos = scanView.eye;
        const int numObj = int(m_objs.size());

        getCompute()->getCommandList()->clearBufferUInt(m_pntOutGP, 0u);
        gvdb::launchKernel(getCompute()->getCommandList(), m_scanKernel,
                           gvdb::groupsFor(m_scanKernel, dm::uint3(uint32_t(m_scanRes.x), uint32_t(m_scanRes.y), 1u)),
                           info, pos, m_scanRes, numObj, tmax, gvdb::deviceAddress(m_objsGP),
                           gvdb::deviceAddress(m_pntsGP), gvdb::deviceAddress(m_clrsGP), gvdb::deviceAddress(m_seedsGP),
                           gvdb::deviceAddress(m_pntOutGP));

        // count hit points
        getCompute()->getCommandList()->copyBuffer(m_pntOutStaging, 0, m_pntOutGP, 0, sizeof(int));
        if (m_showPoints) {
            getCompute()->getCommandList()->copyBuffer(m_pntsStaging, 0, m_pntsGP, 0, size_t(m_numPnts) * sizeof(dm::float3));
            getCompute()->getCommandList()->copyBuffer(m_clrsStaging, 0, m_clrsGP, 0, size_t(m_numPnts) * sizeof(uint32_t));
        }
        syncCompute(getCompute());
        void *data = nullptr;
        data = getCudaDevice()->mapBuffer(m_pntOutStaging, nvrhi::CpuAccessMode::Read);
        if (data) {
            m_totalPnts += *static_cast<const int *>(data);
            getCudaDevice()->unmapBuffer(m_pntOutStaging);
        }
    }

    // Newly activated bricks start at the "far" level set value (and black):
    // the reference filled its pre-allocated atlas once, here each new brick
    // is initialised when it gets its atlas slot.
    void fillNewBricks() {
        const uint64_t leaves = m_volume->getNumNodes(0);
        if (leaves < m_filledBricks.size()) m_filledBricks.clear();   // topology was rebuilt
        m_filledBricks.resize(size_t(leaves), 0);
        const dm::uint3 bwa = m_volume->getBrickDimWithApron();
        const int apron = int(m_volume->getAtlasConfig().apron);
        int filled = 0;
        for (uint64_t n = 0; n < leaves; ++n) {
            if (m_filledBricks[size_t(n)]) continue;
            gvdb::Node *node = m_volume->getNode(0, n);
            if (!node->flags || node->value.x < 0) continue;
            dm::int3 dst = node->value - dm::int3(apron);
            m_volume->copyAtlasBlock(0, dst, m_fillFar, bwa);
            if (m_useColor) m_volume->copyAtlasBlock(1, dst, m_fillZero, bwa);
            m_filledBricks[size_t(n)] = 1;
            ++filled;
        }
        if (filled) log::debug("point-fusion: %d new bricks", filled);
    }

    // ---- fusion (render_update) ----

    void fuse() {
        // dynamic topology
        m_points->accumulateTopology(uint32_t(m_numPnts), m_radius * 2.0f, m_origin);
        m_volume->finishTopology(true);
        m_volume->updateAtlas();
        fillNewBricks();

        // points to level set
        int scPntLen = 0;
        const int subcellSize = 4;
        m_points->insertPointsSubcell(subcellSize, uint32_t(m_numPnts), m_radius * 2.0f, m_origin, scPntLen);
        m_points->gatherLevelSet(subcellSize, uint32_t(m_numPnts), m_radius, m_origin, scPntLen, 0, 1, true);   // true = accumulate
        m_ops->updateApron(0, 0.0f);

        gvdb::GVDBUsage u = m_volume->getUsage();
        char buf[256];
        snprintf(buf, sizeof(buf), "%6.0f / %6.0f MB (%4.3f%%)", u.gpuTotalMB - u.gpuFreeMB, u.gpuTotalMB,
                 u.gpuTotalMB > 0.f ? (u.gpuTotalMB - u.gpuFreeMB) * 100.f / u.gpuTotalMB : 0.f);
        m_memText = buf;
        snprintf(buf, sizeof(buf), "%d x %d x %d", u.extents.x, u.extents.y, u.extents.z);
        m_extText = buf;
        snprintf(buf, sizeof(buf), "%llu brk, %.0fM vox, %4.3f%%", (unsigned long long)u.numBricks, u.millionVoxels,
                 u.occupancyPercent);
        m_voxText = buf;
        snprintf(buf, sizeof(buf), "%6.2fM pnts", float(m_totalPnts) / 1000000.0f);
        m_pntText = buf;
    }

    // ---- overlays ----

    void drawObjects() {
        for (const ScanObj &o : m_objs) getDebugDraw()->box(o.pos, o.pos + o.size, unpackColorA(o.clr));
    }

    void drawPoints() {
        void *pdata = nullptr, *cdata = nullptr;
        pdata = getCudaDevice()->mapBuffer(m_pntsStaging, nvrhi::CpuAccessMode::Read);
        cdata = getCudaDevice()->mapBuffer(m_clrsStaging, nvrhi::CpuAccessMode::Read);
        if (pdata && cdata) {
            const dm::float3 *pnts = static_cast<const dm::float3 *>(pdata);
            const uint32_t *clrs = static_cast<const uint32_t *>(cdata);
            for (int n = 0; n < m_numPnts; ++n) {
                if (pnts[n].x == 0.f && pnts[n].y == 0.f && pnts[n].z == 0.f) continue;   // miss
                // the reference drew a 0.05 box per point; one voxel-long tick is visible
                getDebugDraw()->line(pnts[n], pnts[n] + dm::float3(0.f, 1.f, 0.f), unpackColorA(clrs[n]));
            }
        }
        if (pdata) getCudaDevice()->unmapBuffer(m_pntsStaging);
        if (cdata) getCudaDevice()->unmapBuffer(m_clrsStaging);
    }

    void drawCamera() {
        RenderView scanView = makeRenderView(m_scanCamera, getLight(), m_scanRes.x, m_scanRes.y);
        dm::float3 eye = scanView.eye;
        getDebugDraw()->line(dm::float3(eye.x, 0.f, eye.z), eye, {.5f, .5f, .5f, 1.f});
        getDebugDraw()->frustum(eye, scanView.rayTopLeft, scanView.rayU, scanView.rayV, 60.f * GRID_SCALE,
                                {1.f, 1.f, 0.f, 1.f});
    }

    // Leaving the first-person view: orbit around the current eye position.
    void switchCamera() {
        if (!m_showPov) {
            OrbitController &cam = getCameraOrbit();
            cam.setOrbit({190.f, 30.f, 0.f}, cam.getPosition(), cam.distance);
        }
    }

    void OnRender(uint32_t width, uint32_t height) override {
        getCamera()->verticalFov = verticalFovFromHorizontal(dm::radians(65.f), float(width) / float(height));

        // move the car
        m_car.moveRelative(0.f, 0.f, -m_speed);
        m_car.apply(m_scanCameraNode);
        if (m_showPov) placeOrbit(getCameraOrbit(), m_car.getPosition(), m_car.angles);
        updateScene();

        if (m_generate) {
            scanBuildings();
            fuse();
            ++m_frame;
            if (isScreenshotRun() && m_frame % 10 == 0)
                log::info("Scan %d: %s; extents %s; %s", m_frame, m_voxText.c_str(), m_extText.c_str(), m_pntText.c_str());
        }

        RenderView view = getRenderView(int(width), int(height));
        m_renderer->getViewParams().sectionPoint = m_origin;
        m_renderer->getViewParams().sectionNormal = {0.f, 0.f, -1.f};
        m_renderer->render(getSceneGraph(), view, getPresenter()->getRenderBuffer(0));

        if (m_showObjs) drawObjects();
        if (m_showPoints && m_generate) drawPoints();
        if (m_showTopo) getDebugDraw()->topology(m_instance);
        if (!m_showPov) drawCamera();
    }

    void OnMouseDrag(int button, int dx, int dy, int mods) override {
        const bool shift = (mods & GLFW_MOD_SHIFT) != 0;
        const bool car = shift || m_showPov;   // shift or first-person view: drive the scanner
        switch (button) {
            case GLFW_MOUSE_BUTTON_LEFT:
                if (car) {
                    dm::float3 angles = m_car.angles;
                    angles.x += float(dx) * 0.2f;
                    angles.y -= float(dy) * 0.2f;
                    m_car.setAngles(angles);
                } else {
                    GVDBApp::OnMouseDrag(button, dx, dy, mods);
                }
                break;
            case GLFW_MOUSE_BUTTON_MIDDLE:
                if (car) {
                    dm::float3 p = m_car.getPosition();
                    p.y = dm::clamp(p.y + float(dy), 0.f, 100.f * GRID_SCALE);
                    placeOrbit(m_car, p, m_car.angles);
                    m_speed = 0.f;
                } else {
                    GVDBApp::OnMouseDrag(button, dx, dy, mods);
                }
                break;
            case GLFW_MOUSE_BUTTON_RIGHT:
                if (car) {
                    m_speed = std::max(0.f, m_speed + float(dy) * 0.1f);
                } else {
                    GVDBApp::OnMouseDrag(button, dx, dy, mods);
                }
                break;
            default: break;
        }
    }

    void OnBuildUI() override {
        ImGui::Text("Memory:  %s", m_memText.c_str());
        ImGui::Text("Extents: %s", m_extText.c_str());
        ImGui::Text("Voxels:  %s", m_voxText.c_str());
        ImGui::Text("Points:  %s", m_pntText.c_str());
        ImGui::Separator();
        ImGui::Checkbox("Scan", &m_generate);
        ImGui::Checkbox("Topology", &m_showTopo);
        ImGui::Checkbox("Objects", &m_showObjs);
        ImGui::Checkbox("Points", &m_showPoints);
        if (ImGui::Checkbox("FPV Cam", &m_showPov)) switchCamera();
        if (ImGui::Checkbox("Color", &m_useColor)) setupChannels();
        ImGui::SliderFloat("Car speed", &m_speed, 0.f, 50.f);
        ImGui::Text("Left: orbit / scanner angles, Middle: pan / height,");
        ImGui::Text("Right: distance / speed (scanner in FPV or with shift)");
        ImGui::Text("Keys: 1 points, 2 topology, space scan");
    }

    bool OnKey(int key, int scancode, int action, int mods) override {
        if (action != GLFW_PRESS) return false;
        switch (key) {
            case GLFW_KEY_1: m_showPoints = !m_showPoints; return true;
            case GLFW_KEY_2: m_showTopo = !m_showTopo; return true;
            case GLFW_KEY_SPACE:
                m_generate = !m_generate;
                if (!m_generate) m_speed = 0.f;
                return true;
            default: return false;
        }
    }

 private:
    nvrhi::AutoPtr<gvdb::IGVDBVolume> m_volume;
    nvrhi::AutoPtr<gvdb::IGVDBVoxelOps> m_ops;
    nvrhi::AutoPtr<gvdb::IGVDBPointOps> m_points;
    nvrhi::AutoPtr<gvdb::GVDBVolumeInstance> m_instance;
    std::unique_ptr<VolRenderer> m_renderer;

    nvrhi::AutoPtr<engine::PerspectiveCamera> m_scanCamera;
    nvrhi::AutoPtr<engine::SceneGraphNode> m_scanCameraNode;
    OrbitController m_car;

    gvdb::IComputeKernel *m_scanKernel = nullptr;
    std::vector<ScanObj> m_objs;
    nvrhi::BufferHandle m_objsGP;
    nvrhi::BufferHandle m_pntsGP, m_pntsStaging;
    nvrhi::BufferHandle m_clrsGP, m_clrsStaging;
    nvrhi::BufferHandle m_pntOutGP, m_pntOutStaging;
    nvrhi::BufferHandle m_seedsGP;
    nvrhi::BufferHandle m_fillFar, m_fillZero;
    std::vector<uint8_t> m_filledBricks;
    std::mt19937 m_rng{1043};

    dm::int3 m_scanRes = {240, 180, 0};
    int m_maxPnts = 1000000;
    int m_numPnts = 0;
    long long m_totalPnts = 0;
    float m_gridSize = 0.f;         // city block pitch (metres)
    dm::float3 m_cityCenter = dm::float3::zero();
    float m_radius = 1.f;           // m_radius of the reference
    dm::float3 m_origin = dm::float3::zero();
    float m_speed = 0.f;
    int m_frame = 0;
    bool m_generate = true;
    bool m_showTopo = false;
    bool m_showObjs = false;
    bool m_showPoints = false;
    bool m_showPov = true;
    bool m_useColor = false;
    std::string m_memText, m_extText, m_voxText, m_pntText;
};

int main(int argc, const char **argv) {
    float speed = 0.f;
    bool pov = true;
    bool color = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc) speed = float(atof(argv[++i]));
        else if (strcmp(argv[i], "--orbit") == 0) pov = false;
        else if (strcmp(argv[i], "--color") == 0) color = true;
    }

    GVDBAppOptions options;
    options.title = "GVDB Voxels - point-fusion";
    options.sampleName = GVDB_SAMPLE_NAME;
    return runGVDBApp(argc, argv, options, [&](app::DeviceManager *dm) -> nvrhi::AutoPtr<GVDBApp> {
        auto app = MAKE_RC_OBJ_PTR(PointFusionApp, dm);
        app->setInitialSpeed(speed);
        app->setFirstPersonView(pov);
        app->setUseColor(color);
        return app;
    });
}

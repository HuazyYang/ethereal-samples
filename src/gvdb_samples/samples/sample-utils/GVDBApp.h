#ifndef SAMPLE_UTILS_GVDBAPP_H
#define SAMPLE_UTILS_GVDBAPP_H
// Application base of the interactive GVDB samples (the role of NVPWindow +
// nv_gui + the GVDB Scene in the reference), on Donut:
//
//   * devices: the nvrhi device of the DeviceManager, a gp CUDA device and
//     queue, the nvrhi<->gp interop device, and (when available) the gp OptiX
//     device
//   * file system: "assets" -> GVDB_SAMPLES_ASSETS_DIR, "ptx" -> <exe>/ptx,
//     "shaders" -> <exe>/shaders/<target>/<dxil|dxbc|spirv>
//   * scene: a GVDBSceneGraph with a PerspectiveCamera leaf and a point Light
//     leaf, both driven by orbit controllers with the reference mouse mapping
//     (left drag = orbit angles, middle = pan, right = distance; shift = light)
//   * presentation: ScreenPresenter target 0 covering the window, DebugDraw
//     overlay, ImGui window ("Inspector") built by OnBuildUI()
//
// A sample derives from GVDBApp, implements the hooks and calls runGVDBApp().
#include <donut/app/DeviceManager.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/ImGuiRenderPass.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/SceneGraph.h>
#include <nvrhi/core/autoptr.h>
#include <gvdb/GPDevice.h>
#include <gvdb/GPDeviceCUDA.h>
#include <gvdb/GPDeviceNVRHI.h>
#include <gvdb/GPDeviceOptiX.h>
#include <gvdb/GVDBScene.h>
#include <sample-utils/SampleTypes.h>
#include <sample-utils/VolRenderer.h>
#include <sample-utils/ScreenPresenter.h>
#include <sample-utils/DebugDraw.h>
#include <memory>
#include <string>

namespace SampleUtils {

// Orbit camera state as in the reference Camera3D: azimuth / elevation angles
// in degrees, target point, orbit distance. apply() writes the node transform
// (translation + rotation looking at the target) of a camera or light leaf.
struct OrbitController {
    dm::float3 angles = {0.f, 45.f, 0.f};   // x = azimuth, y = elevation (degrees)
    dm::float3 target = dm::float3::zero();
    float distance = 120.f;

    dm::float3 getPosition() const;
    void setOrbit(dm::float3 anglesDeg, dm::float3 targetPos, float dist);
    // Keeps the position; re-aims at a new target distance along the angles.
    void setAngles(dm::float3 anglesDeg);
    void setPosition(dm::float3 position);
    // Moves target and position by (dx, dy, dz) in the camera's right / up / back axes.
    void moveRelative(float dx, float dy, float dz);
    void apply(donut::engine::SceneGraphNode *node) const;
};

class GVDBApp : public donut::app::IRenderPass {
    NVRHI_INHERIT_INTERFACE_TABLE()
 public:
    explicit GVDBApp(donut::app::DeviceManager *deviceManager);
    ~GVDBApp() override;

    // Name of the sample (its directory under <exe>/shaders/ when it has its own
    // shaders, mounted as "shaders/<name>"); set before Init(), runGVDBApp does it.
    void setSampleName(const char *name) { m_sampleName = name ? name : ""; }
    const std::string &getSampleName() const { return m_sampleName; }

    // Creates devices, file system, scene graph, camera and light, presenter;
    // then calls OnInit(). Returns false on failure.
    bool Init(bool wantOptix = false);

    // ---- hooks ----
    virtual bool OnInit() = 0;
    virtual void OnUpdate(float elapsedSeconds) {}
    // Produce the frame on the gp queue into getPresenter()->getRenderBuffer(0)
    // (and any further targets); called between beginGPFrame / endGPFrame.
    virtual void OnRender(uint32_t width, uint32_t height) = 0;
    // Graphics-side additions inside the presented frame (extra blits, debug lines via getDebugDraw()).
    virtual void OnDrawOverlay(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer) {}
    virtual void OnBuildUI() {}
    virtual void OnResize(uint32_t width, uint32_t height) {}
    virtual bool OnKey(int key, int scancode, int action, int mods) { return false; }
    // Mouse drag with a button held (dx, dy in pixels). Default: orbit controls.
    virtual void OnMouseDrag(int button, int dx, int dy, int mods);
    virtual bool OnMouseButton(int button, int action, int mods) { return false; }

    // ---- accessors ----
    donut::vfs::RootFileSystem *getVFS() const { return m_vfs.Get(); }
    donut::engine::ShaderFactory *getShaderFactory() const { return m_shaderFactory.Get(); }
    donut::gp::IDevice *getGPDevice() const { return m_gpDevice.Get(); }
    donut::gp::IDeviceQueue *getGPQueue() const { return m_gpQueue.Get(); }
    donut::IGPAndNVRHIInteropDevice *getInteropDevice() const { return m_interopDevice.Get(); }
    donut::gp::IRTDevice *getRTDevice() const { return m_rtDevice.Get(); }   // nullptr without OptiX
    gvdb::GVDBSceneGraph *getSceneGraph() const { return m_sceneGraph.Get(); }
    donut::engine::PerspectiveCamera *getCamera() const { return m_camera.Get(); }
    donut::engine::SceneGraphNode *getCameraNode() const { return m_cameraNode.Get(); }
    donut::engine::Light *getLight() const { return m_light.Get(); }
    donut::engine::SceneGraphNode *getLightNode() const { return m_lightNode.Get(); }
    OrbitController &getCameraOrbit() { return m_cameraOrbit; }
    OrbitController &getLightOrbit() { return m_lightOrbit; }
    // Applies the orbit controllers to the camera / light nodes and refreshes the graph.
    void updateScene();
    RenderView getRenderView(int width, int height) const;
    ScreenPresenter *getPresenter() const { return m_presenter.get(); }
    DebugDraw *getDebugDraw() const { return m_debugDraw.get(); }
    dm::int2 getMousePos() const { return m_mousePos; }
    int getMouseButtonDown() const { return m_mouseDown; }   // -1 = none
    uint32_t getFrameCounter() const { return m_frameCounter; }
    // Creates a volume with the app's queue / file system.
    nvrhi::AutoPtr<gvdb::IGVDBVolume> createVolume();
    // Attaches a volume instance under a new node (index -> world transform) of the root.
    nvrhi::AutoPtr<gvdb::GVDBVolumeInstance> addVolumeInstance(gvdb::IGVDBVolume *volume, const dm::affine3 &indexToWorld,
                                                              const std::string &name);
    std::filesystem::path getAssetPath(const char *name) const;

    // Unattended run: after `frames` presented frames the back buffer (without
    // the ImGui overlay) is saved as PNG and the window closes. runGVDBApp sets
    // it from `--screenshot <file.png> [--frames N]` (default 8 frames).
    void setScreenshot(const std::filesystem::path &path, uint32_t frames) {
        m_screenshotPath = path;
        m_screenshotFrames = frames;
    }
    bool isScreenshotRun() const { return !m_screenshotPath.empty(); }

    // ---- IRenderPass ----
    void Render(nvrhi::IFramebuffer *framebuffer) override;
    void Animate(float elapsedTimeSeconds) override;
    void BackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) override;
    bool KeyboardUpdate(int key, int scancode, int action, int mods) override;
    bool MousePosUpdate(double xpos, double ypos) override;
    bool MouseButtonUpdate(int button, int action, int mods) override;

    // Internal: the ImGui pass forwards to OnBuildUI().
    void buildUI() { OnBuildUI(); }

 protected:
    nvrhi::AutoPtr<donut::vfs::RootFileSystem> m_vfs;
    nvrhi::AutoPtr<donut::engine::ShaderFactory> m_shaderFactory;
    nvrhi::AutoPtr<donut::gp::IDevice> m_gpDevice;
    nvrhi::AutoPtr<donut::gp::IDeviceQueue> m_gpQueue;
    nvrhi::AutoPtr<donut::IGPAndNVRHIInteropDevice> m_interopDevice;
    nvrhi::AutoPtr<donut::gp::IRTDevice> m_rtDevice;
    nvrhi::AutoPtr<gvdb::GVDBSceneGraph> m_sceneGraph;
    nvrhi::AutoPtr<donut::engine::PerspectiveCamera> m_camera;
    nvrhi::AutoPtr<donut::engine::SceneGraphNode> m_cameraNode;
    nvrhi::AutoPtr<donut::engine::Light> m_light;
    nvrhi::AutoPtr<donut::engine::SceneGraphNode> m_lightNode;
    OrbitController m_cameraOrbit;
    OrbitController m_lightOrbit;
    std::unique_ptr<ScreenPresenter> m_presenter;
    std::unique_ptr<DebugDraw> m_debugDraw;
    nvrhi::CommandListHandle m_commandList;
    dm::int2 m_mousePos = dm::int2::zero();
    int m_mouseDown = -1;
    int m_mouseMods = 0;
    uint32_t m_frameCounter = 0;
    std::string m_sampleName;
    std::filesystem::path m_screenshotPath;
    uint32_t m_screenshotFrames = 8;

    void captureScreenshot(nvrhi::IFramebuffer *framebuffer);
};

struct GVDBAppOptions {
    const char *title = "GVDB sample";
    const char *sampleName = nullptr;   // GVDB_SAMPLE_NAME of the executable (per-sample shader mount)
    uint32_t width = 1024;
    uint32_t height = 768;
    bool wantOptix = false;    // create the OptiX device (sample still runs without it)
    bool vsync = false;
};

// Creates the window / device (API and --debug from the command line), the app
// through `factory`, its ImGui pass, and runs the message loop. Returns the exit code.
int runGVDBApp(int argc, const char *const *argv, const GVDBAppOptions &options,
               const std::function<nvrhi::AutoPtr<GVDBApp>(donut::app::DeviceManager *)> &factory);

template <class TApp>
int runGVDBApp(int argc, const char *const *argv, const GVDBAppOptions &options) {
    return runGVDBApp(argc, argv, options, [](donut::app::DeviceManager *dm) -> nvrhi::AutoPtr<GVDBApp> {
        return nvrhi::TakeOver(MAKE_RC_OBJ(TApp, dm));
    });
}

}  // namespace SampleUtils

#endif /* SAMPLE_UTILS_GVDBAPP_H */

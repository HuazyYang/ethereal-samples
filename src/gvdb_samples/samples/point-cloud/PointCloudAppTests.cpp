// Independent application frame/input/scene-mapping fixtures.
// SPDX-License-Identifier: Apache-2.0
#include "PointCloudAppTests.h"
#include "PointCloudAppScene.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace gvdb::samples::pointcloud {
namespace {
void require(bool v, const char* what) {
    if (!v)
        throw std::runtime_error(what);
}
void expectNear(float actual, float expected, const char* what) {
    require(std::isfinite(actual) && std::abs(actual - expected) < 1e-4f, what);
}
void word(std::ofstream& out, uint32_t value) {
    for (int i = 0; i < 4; ++i)
        out.put(char(value >> (8 * i)));
}
void scalar(std::ofstream& out, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    word(out, bits);
}
void pointFile(const std::filesystem::path& path, float x) {
    std::ofstream out(path, std::ios::binary);
    word(out, 1);
    for (float value : {x, 4.f, 4.f, x, 4.f, 4.f})
        scalar(out, value);
    for (int i = 0; i < 6; ++i)
        out.put(char(0));
}
struct TempFiles {
    std::vector<std::filesystem::path> files;
    ~TempFiles() {
        for (const auto& p : files) {
            std::error_code error;
            std::filesystem::remove(p, error);
        }
    }
};
}  // namespace
void runPointCloudAppTests(ethereal::slang::ComputeRuntime& runtime) {
    const auto prefix =
        (std::filesystem::temp_directory_path() /
         ("gvdb-point-app-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
            .string();
    TempFiles temporary{
        {prefix + "0.dat", prefix + "1.dat", prefix + "0.obj", prefix + "1.obj"}
    };
    pointFile(temporary.files[0], 4);
    pointFile(temporary.files[1], -20);
    {
        std::ofstream out(temporary.files[2]);
        out << "v 0 0 0\nv 4 0 0\nv 0 4 0\nvn 0 0 1\nf 1//1 2//1 3//1\n";
    }
    {
        std::ofstream out(temporary.files[3]);
        out << "v 10 0 0\nv 14 0 0\nv 10 4 0\nvn 0 0 1\nf 1//1 2//1 3//1\n";
    }
    AppOptions options;
    options.compute = true;
    options.assets = std::filesystem::temp_directory_path();
    auto& s = options.scene;
    s.pointsOn = true;
    s.pointPath = prefix;
    s.pointFile = "%d.dat";
    s.frame = 0;
    s.frameStep = 1;
    s.renderScale = 2;
    s.maxSamples = 2;
    s.cameraAngles = {0, 0, 0};
    s.cameraTarget = {4, 4, 4};
    s.cameraDistance = 20;
    s.materials.emplace_back();
    SceneModel model;
    model.file = temporary.files[2].string();
    model.scale = 2;
    model.offset = {1, 2, 3};
    s.models.push_back(model);
    s.polysOn = true;
    s.polyPath = prefix;
    s.polyFile = "%d.obj";
    s.polyFrame = 0;
    s.polyFrameStep = 1;
    s.polyScale = 3;
    s.polyOffset = {-1, 0, 0};
    PointCloudAppScene scene(runtime, options);
    sample::VolumeViewOptions view;
    view.width = view.height = 64;
    scene.animate(0, view);
    require(scene.frame() == 0 && scene.reconstruction().generation() == 1,
            "Initial packed frame loaded exactly once");
    expectNear(view.cameraEye.z, 48, "Scaled camera eye");
    expectNear(view.cameraForward.z, -1, "Original orbit forward");
    expectNear(view.cameraNear, 1, "Original near plane");
    expectNear(view.cameraFar, 10000, "Original far plane");
    const auto& description = scene.sceneDescription();
    require(
        description.volumes.size() == 1 && description.meshes.size() == 2 && description.instances.size() == 3,
        "Mixed scene geometry mapping");
    expectNear(description.meshes[0].positions[0].x, 1, "Model baked offset");
    expectNear(description.meshes[0].positions[1].x, 9, "Model baked scale then offset");
    expectNear(description.instances[1].transform[0], 2, "Separate render scale instance");
    require(
        description.volumes[0].transfer.size() == 16384 && description.volumes[0].transferLastIndex == 16300,
        "Legacy transfer table endpoint");
    expectNear(description.volumes[0].transfer[8192].y, 1, "Transfer midpoint");
    expectNear(description.volumes[0].threshold.w, 3, "Missing level-set scalar");
    expectNear(description.meshes[1].positions[0].x, -1, "Initial polygon-series transform");
    auto image = sample::renderVolumeView(runtime, scene, view);
    scene.animate(0, view);
    require(scene.frame() == 0 && scene.sampleCount() == 1, "Dataset held between progressive samples");
    image = sample::renderVolumeView(runtime, scene, view);
    scene.animate(0, view);
    require(scene.frame() == 1 && scene.sampleCount() == 0 && scene.reconstruction().generation() == 2,
            "Dataset advances after maxSamples");
    require(scene.polygonFrame() == 1, "Polygon frame advances independently");
    expectNear(scene.sceneDescription().meshes[1].positions[0].x, 29,
               "Polygon geometry replaced with next frame");
    require(scene.packed().volume.boundsMax.x < 0, "Frame replacement retains no old positive topology");
    require(scene.key('1', view) && scene.overlayLines().size() == 1, "Point-overlay key and glyph");
    require(scene.key('2', view) && view.topology, "Topology toggle");
    require(scene.key('V', view) && scene.key('I', view), "Unsupported generic modes consumed");
    const auto eye = view.cameraEye;
    require(scene.mouseDrag(0, 10, 5, 0, view), "Orbit input consumed");
    require(view.cameraEye.x != eye.x, "Orbit changes camera");
    const auto orbit = view.cameraEye;
    require(scene.mouseDrag(2, 8, -4, 0, view), "Pan input consumed");
    require(view.cameraEye.y != orbit.y, "Pan moves camera target");
    const auto light = view.lightDirection;
    require(scene.mouseDrag(0, 15, 0, 1, view), "Shift-light orbit consumed");
    require(view.lightDirection.x != light.x, "Light moves independently");
    require(scene.sampleCount() == 0, "Interactive camera/light reset samples");
    options.captureSamples = 3;
    PointCloudAppScene capture(runtime, options);
    sample::VolumeViewOptions captureView;
    captureView.width = captureView.height = 32;
    for (unsigned i = 0; i < 3; ++i) {
        capture.animate(0, captureView);
        sample::renderVolumeView(runtime, capture, captureView);
    }
    capture.animate(0, captureView);
    require(capture.frame() == 0 && capture.polygonFrame() == 0 && capture.reconstruction().generation() == 1 &&
                capture.sampleCount() == 3,
            "Sample capture freezes time-series advancement");
    options.keyAnimation = true;
    options.scene.frame = 1220;
    options.scene.pointFile = "0.dat";
    options.scene.polysOn = false;
    PointCloudAppScene animated(runtime, options);
    animated.animate(0, captureView);
    const float pitch = dm::radians(29.8f);
    expectNear(captureView.cameraEye.x, 602, "Original keyframe target interpolation");
    require(std::abs(captureView.cameraEye.y - (-201.f + std::sin(pitch) * 3732.f)) < .001f,
            "Original keyframe distance and elevation interpolation");
    std::cout << "Point-cloud application input/frame/scene fixtures PASS\n";
}
void runPointCloudRayPolicyTests(ethereal::slang::ComputeRuntime& runtime) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("gvdb-point-ray-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".obj");
    TempFiles temporary{{path}};
    AppOptions options;
    options.compute = false;
    auto& scene = options.scene;
    scene.renderScale = 1;
    scene.cameraAngles = {0, 0, 0};
    scene.cameraTarget = dm::float3::zero();
    scene.cameraDistance = 2;
    scene.maxSamples = 3;
    scene.frameStep = 1;
    sample::SceneMaterial material;
    material.ambColor = {.25f, 0, 0};
    material.diffColor = material.specColor = dm::float3::zero();
    scene.materials.push_back(material);
    SceneModel model;
    model.file = path.string();
    scene.models.push_back(model);
    for (float z : {-12000.f, 1.75f}) {
        {
            std::ofstream out(path);
            out << "v -20000 -20000 " << z << "\nv 20000 -20000 " << z << "\nv 0 20000 " << z
                << "\nvn 0 0 1\nf 1//1 2//1 3//1\n";
        }
        PointCloudAppScene app(runtime, options);
        sample::VolumeViewOptions view;
        view.width = 17;
        view.height = 13;
        app.animate(0, view);
        const auto image = sample::renderVolumeView(runtime, app, view);
        std::vector<float> depths(size_t(image.width) * image.height);
        std::vector<slang::Float4> pixels(depths.size());
        runtime.read(image.depth, depths.data(), depths.size() * sizeof(float));
        runtime.read(image.color, pixels.data(), pixels.size() * sizeof(slang::Float4));
        const size_t center = size_t(image.height / 2) * image.width + image.width / 2;
        const float d = depths[center];
        if (z < 0)
            require(d > view.cameraFar && d < 13000, "Legacy RT retains geometry beyond raster far plane");
        else
            require(d > .2f && d < view.cameraNear, "Legacy RT retains geometry before raster near plane");
        expectNear(pixels[center].x, .25f, "Unclipped scene material reaches the camera");
        app.animate(0, view);
        sample::renderVolumeView(runtime, app, view);
        require(app.sampleCount() == 2 && app.frame() == 0, "Second RT sample retains dataset");
        view.width = 23;
        view.height = 19;
        const auto resized = sample::renderVolumeView(runtime, app, view);
        require(resized.width == 23 && resized.height == 19 && app.sampleCount() == 1,
                "Resize resets actual RT accumulation and output dimensions");
        app.animate(0, view);
        require(app.frame() == 0 && app.sampleCount() == 1,
                "Resize does not advance the dataset using the old sample count");
        sample::renderVolumeView(runtime, app, view);
        app.animate(0, view);
        require(app.frame() == 0 && app.sampleCount() == 2, "Resized accumulation retains dataset");
        sample::renderVolumeView(runtime, app, view);
        app.animate(0, view);
        require(app.frame() == 1 && app.sampleCount() == 0,
                "Dataset advances only after three samples at the new resolution");
    }
    std::cout << "Point-cloud RT near/far policy and resize accumulation PASS\n";
}
}  // namespace gvdb::samples::pointcloud

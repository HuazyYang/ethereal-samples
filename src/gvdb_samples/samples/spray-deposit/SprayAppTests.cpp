// Application controls and five-mode real-asset rendering through the shared viewer.
// SPDX-License-Identifier: Apache-2.0
#include "SprayAppTests.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>
namespace gvdb::samples::spray {
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void close(float value, float expected, const char* message) {
    require(std::isfinite(value) && std::abs(value - expected) < .001f, message);
}
}  // namespace
void runSprayAppTests(ethereal::slang::ComputeRuntime& runtime, const std::filesystem::path& asset) {
    SprayOptions options;
    options.asset = asset;
    options.simulate = false;
    SprayScene scene(runtime, options);
    sample::VolumeViewOptions view;
    view.width = 64;
    view.height = 48;
    scene.animate(0, view);
    require(scene.simulation().time() == 0 && view.explicitCamera, "Paused initialization does not deposit");
    close(view.cameraEye.x, 200 + 1000 * std::cos(dm::radians(40.f)) * std::sin(dm::radians(45.f)),
          "Original camera yaw/distance");
    close(view.cameraEye.y, 200 + 1000 * std::sin(dm::radians(40.f)), "Original camera elevation");
    close(view.horizontalFov, 50, "Original horizontal FOV");
    view.showUI = true;
    require(scene.key('`', view) && !view.showUI, "Inspector hide key");
    require(scene.key('`', view) && view.showUI, "Inspector restore key");
    require(scene.key('1', view), "Simulation key handled");
    scene.animate(0, view);
    require(scene.simulation().time() == 1 && scene.simulation().hitCount() > 0,
            "One animation advances exactly one deposition frame");
    require(scene.overlayLines().size() == scene.simulation().hitCount(), "One overlay per valid spray hit");
    scene.key('3', view);
    require(scene.overlayLines().empty(), "Ray display toggle");
    scene.key('3', view);
    scene.key(' ', view);
    scene.animate(0, view);
    require(scene.simulation().time() == 1 && scene.overlayLines().empty(),
            "Pause freezes wand and hides rays");
    require(scene.key('2', view) && view.topology, "Topology enabled");
    scene.key('2', view);
    require(!view.topology, "Topology disabled");
    require(scene.key('V', view) && scene.key('I', view) && view.mode == sample::VolumeViewMode::Volume &&
                !view.sectionInset,
            "Generic modes cannot override spray shading");
    const dm::float3 origin(view.cameraEye.x, view.cameraEye.y, view.cameraEye.z),
        forward(view.cameraForward.x, view.cameraForward.y, view.cameraForward.z),
        up(view.cameraUp.x, view.cameraUp.y, view.cameraUp.z);
    const auto right = dm::normalize(dm::cross(forward, up));
    const float horizontal = std::tan(dm::radians(view.horizontalFov) * .5f),
                vertical = horizontal * float(view.height) / view.width;
    std::vector<slang::Float4> origins, directions;
    for (uint32_t y = 0; y < view.height; ++y)
        for (uint32_t x = 0; x < view.width; ++x) {
            const auto d =
                dm::normalize(forward + right * ((2 * (float(x) + .5f) / view.width - 1) * horizontal) +
                              up * ((1 - 2 * (float(y) + .5f) / view.height) * vertical));
            origins.push_back({origin.x, origin.y, origin.z, 0});
            directions.push_back({d.x, d.y, d.z, 0});
        }
    auto originBuffer = runtime.buffer(origins.size() * 16, 16, origins.data()),
         directionBuffer = runtime.buffer(directions.size() * 16, 16, directions.data());
    auto query = slang::queryVolumeRays(runtime, scene.volume(), originBuffer, directionBuffer,
                                        uint32_t(origins.size()));
    std::vector<slang::Float4> queryHits(origins.size());
    runtime.read(query.hits, queryHits.data(), queryHits.size() * 16);
    size_t queryHitCount = 0, queryLimits = 0;
    for (const auto& hit : queryHits) {
        queryHitCount += hit.w > 0;
        queryLimits += hit.w < 0;
    }
    std::printf("Metal camera ray query hits%zu limits%zu\n", queryHitCount, queryLimits);
    require(queryLimits == 0, "Real asset camera rays must never exhaust traversal due to rounded entry cells");
    for (unsigned mode = 0; mode < 5; ++mode) {
        while (uint32_t(scene.shading()) != mode)
            scene.key('4', view);
        auto image = sample::renderVolumeView(runtime, scene, view);
        std::vector<slang::Float4> pixels(size_t(image.width) * image.height);
        std::vector<float> depth(pixels.size());
        runtime.read(image.color, pixels.data(), pixels.size() * 16);
        runtime.read(image.depth, depth.data(), depth.size() * 4);
        size_t hits = 0;
        for (size_t i = 0; i < pixels.size(); ++i) {
            const auto p = pixels[i];
            require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::isfinite(p.w) &&
                        !std::isnan(depth[i]) && depth[i] >= 0,
                    "Finite five-mode application output");
            hits += std::isfinite(depth[i]) && depth[i] >= 0;
            if (mode == 0)
                require(p.x == 0 && p.y == 0 && p.z == 0 && p.w == 0 && std::isinf(depth[i]),
                        "Off has black transparent color and no geometry depth");
        }
        // The fixed X=50 section may lie beyond this part for the initial camera.
        // Its nonempty plane equation is independently tested in SprayTests.
        if (mode == 1 || mode == 2 || mode == 4)
            require(hits > 0, "Real voxel/surface/volume mode must contain the metal part");
        std::printf("Spray mode%u image hits%zu\n", mode, hits);
    }
    const auto eye = view.cameraEye;
    require(scene.mouseDrag(0, 10, 5, 0, view) && view.cameraEye.x != eye.x, "Orbit control changes camera");
    const auto orbit = view.cameraEye;
    require(scene.mouseDrag(2, 8, -4, 0, view) && view.cameraEye.y != orbit.y, "Pan control changes target");
    const auto zoom = view.cameraEye;
    require(scene.mouseScroll(1, view) && view.cameraEye.x != zoom.x, "Wheel zoom control");
    const auto light = view.lightDirection, camera = view.cameraEye;
    require(
        scene.mouseDrag(0, 15, 0, 1, view) && view.lightDirection.x != light.x && view.cameraEye.x == camera.x,
        "Shift drag moves only light");
    for (unsigned i = 0; i < 3; ++i) {
        scene.key('5', view);
        scene.key('1', view);
        scene.animate(0, view);
        scene.key('1', view);
    }
    require(scene.simulation().time() == 4, "Three wand choices each advance one frame");
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.DisplaySize = {640, 480};
    io.DeltaTime = 1.f / 60;
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    for (int frame = 0; frame < 2; ++frame) {
        ImGui::NewFrame();
        ImGui::Begin("Inspector");
        scene.buildUI(view);
        ImGui::End();
        ImGui::Render();
    }
    const bool inspector = ImGui::GetDrawData() && ImGui::GetDrawData()->TotalVtxCount > 0;
    ImGui::DestroyContext();
    require(inspector, "Inspector emits controls without a native window");
    std::puts("Spray pause/keys/wands/overlays/camera/light/Inspector/five-mode application tests PASS");
}
}  // namespace gvdb::samples::spray

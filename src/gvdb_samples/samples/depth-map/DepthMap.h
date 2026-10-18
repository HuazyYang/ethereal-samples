// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <donut/core/math/math.h>
#include <filesystem>
#include <gvdb/slang/GVDBPortableVbx.h>
#include <memory>
#include <nvrhi/graphics-interop.h>
#include <vector>
namespace depth_map
{
struct Mesh
{
    std::vector<dm::float3> positions, normals;
    std::vector<uint32_t> indices;
};
struct Scene
{
    gvdb::slang::VbxVolume volume;
    Mesh mesh;
    std::vector<gvdb::slang::Float4> transfer;
    bool refreshApron = true;
};
struct View
{
    dm::affine3 cameraToWorld = dm::affine3::identity(); // +Z camera forward
    dm::affine3 indexToWorld = dm::affine3::identity();
    dm::float3 light{0, 0, 0};
    float nearPlane = .1f, farPlane = 1000, step = .5f, bias = 0;
    bool exactDepth = false;
};
struct Images
{
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> color, polygon;
    std::vector<float> depth;
    std::vector<uint32_t> volume;
};
Scene loadScene(const std::filesystem::path &volume, const std::filesystem::path &mesh);
std::vector<gvdb::slang::Float4> referenceTransfer();
dm::affine3 orbitCamera(dm::float3 angles, dm::float3 target, float distance);
dm::affine3 volumeTransform(dm::float3 angles, dm::float3 translation);
View defaultView();
class Renderer
{
    struct Impl;
    std::unique_ptr<Impl> m_impl;

  public:
    Renderer(nvrhi::IDevice *graphics, nvrhi::IDevice *compute, const Scene &scene);
    ~Renderer();
    void setMesh(const Mesh &mesh);
    // No host synchronization except on allocation/resize; all frame transfers use GPU waits/signals.
    void render(uint32_t width, uint32_t height, const View &view, nvrhi::IFramebuffer *target = nullptr);
    Images read(); // Explicit test/screenshot readback, never used by interactive frames.
};
void runAnalyticTests(nvrhi::IDevice *graphics, nvrhi::IDevice *compute);
} // namespace depth_map

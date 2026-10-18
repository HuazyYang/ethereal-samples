// SPDX-License-Identifier: Apache-2.0
#include "PrintTests.h"
#include "PrintVolume.h"
#include "VolumeViewer.h"
#include "../../tests/VoxelizeFixtures.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
namespace gvdb::samples {
namespace {
using namespace slang;
class CubeScene final : public sample::VolumeScene {
    PrintVolume m_pipeline;

 public:
    CubeScene(ethereal::slang::ComputeRuntime& runtime, const SampleUtils::TriangleMesh& mesh,
              PrintConfiguration c)
        : m_pipeline(runtime, mesh, c) {}
    portable::SnapshotHandle ownedSnapshot() const override { return m_pipeline.snapshot(); }
    std::vector<Float4> transfer() const override {
        return {
            {0, 0, 0, 1},
            {1, 1, 1, 1}
        };
    }
    PrintVolume& pipeline() { return m_pipeline; }
};
float cubeValue(int x, int y, int z) {
    if (x < 1 || x > 4 || y < 1 || y > 4 || z < 1 || z > 4)
        return 0;
    return x == 1 || x == 4 || y == 1 || y == 4 || z == 1 || z == 4 ? 1.f : .5f;
}
void testCube(ethereal::slang::ComputeRuntime& runtime, Float4 translation) {
    fixtures::VoxelizeFixture fixture;
    SampleUtils::TriangleMesh mesh;
    for (auto v : fixture.vertices)
        mesh.positions.push_back({v.x, v.y, v.z});
    for (size_t i = 0; i < fixture.indices.size(); i += 3)
        mesh.indices.push_back(
            {int(fixture.indices[i]), int(fixture.indices[i + 1]), int(fixture.indices[i + 2])});
    PrintConfiguration c;
    c.voxelSize = 1;
    c.partSize = 1;
    c.pivot = translation;
    c.rotationDegrees = 0;
    CubeScene scene(runtime, mesh, c);
    auto& pipeline = scene.pipeline();
    const auto r = pipeline.resolution();
    if (r.x != 32 || r.y != 32 || r.z != 32 || pipeline.surfaceCount() != 56 || pipeline.interiorCount() != 8)
        throw std::runtime_error("Print analytic cube dimensions or surface/interior counts differ");
    for (int z = 0; z < 32; ++z)
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
                if (pipeline.dense()[size_t((z * 32 + y) * 32 + x)] != cubeValue(x, y, z))
                    throw std::runtime_error("Print cube dense voxel classification differs");
    const auto snapshot = pipeline.snapshot()->getDesc();
    portable::LevelInfo level;
    if (NVRHI_FAILED(pipeline.snapshot()->getLevel(0, level)))
        throw std::runtime_error("Print snapshot level");
    if (level.nodeExtent.x != 32 || snapshot.volume.atlasBrickCountAndWidth.w != 34)
        throw std::runtime_error("Legacy fromBranching(...,5) requires leaf32/apron34");
    if (snapshot.activeLeafCount != 1)
        throw std::runtime_error("Print cube did not activate exactly one leaf");
    const auto origin = pipeline.origin();
    const auto maps = pipeline.readAtlasMap();
    const auto& map = maps.at(0);
    portable::NodeId id;
    portable::NodeId::encode(0, 0, 0, id);
    const auto* leaf = pipeline.topology().getNode(id);
    if (!leaf)
        throw std::runtime_error("Print leaf node");
    const int32_t expectedOrigin[] = {int32_t(translation.x), int32_t(translation.y), int32_t(translation.z)};
    const int32_t actualOrigin[] = {origin.x, origin.y, origin.z}, mapOrigin[] = {map.x, map.y, map.z};
    const float lower[] = {snapshot.volume.boundsMin.x, snapshot.volume.boundsMin.y,
                           snapshot.volume.boundsMin.z};
    const float upper[] = {snapshot.volume.boundsMax.x, snapshot.volume.boundsMax.y,
                           snapshot.volume.boundsMax.z};
    for (size_t axis = 0; axis < 3; ++axis)
        if (actualOrigin[axis] != expectedOrigin[axis] ||
            (axis == 0   ? leaf->position.x
             : axis == 1 ? leaf->position.y
                         : leaf->position.z) != expectedOrigin[axis] ||
            mapOrigin[axis] != expectedOrigin[axis] || lower[axis] != float(expectedOrigin[axis]) ||
            upper[axis] != float(expectedOrigin[axis] + 32))
            throw std::runtime_error(
                "Print signed origin, node, atlas map or world bounds did not preserve translation");
    if (map.leaf != 0)
        throw std::runtime_error("Print atlas map leaf reference differs");
    const auto atlas = pipeline.readAtlas();
    for (int z = 0; z < 34; ++z)
        for (int y = 0; y < 34; ++y)
            for (int x = 0; x < 34; ++x)
                if (atlas[size_t((z * 34 + y) * 34 + x)] != cubeValue(x - 1, y - 1, z - 1))
                    throw std::runtime_error("Print signed sparse topology/apron differs");
    sample::VolumeViewOptions view;
    view.width = 32;
    view.height = 32;
    view.mode = sample::VolumeViewMode::Section2D;
    view.sectionHeight = 3.5f / 32;
    const auto image = sample::renderVolumeView(runtime, scene, view);
    std::vector<Float4> pixels(1024);
    std::vector<float> depth(1024);
    runtime.read(image.color, pixels.data(), pixels.size() * sizeof(Float4));
    runtime.read(image.depth, depth.data(), depth.size() * 4);
    for (int z = 0; z < 32; ++z)
        for (int x = 0; x < 32; ++x) {
            const float expected = cubeValue(x, 3, z);
            const auto& p = pixels[size_t(z * 32 + x)];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(p.w) ||
                !std::isfinite(depth[size_t(z * 32 + x)]) || std::abs(p.x - expected) > 1e-6f ||
                std::abs(p.y - expected) > 1e-6f || std::abs(p.z - expected) > 1e-6f || p.w != 1 ||
                depth[size_t(z * 32 + x)] != 0)
                throw std::runtime_error("Print section pixels differ from analytic cube cross-section");
        }
    const auto retained = pipeline.snapshot();
    bool rejected = false;
    try {
        pipeline.rebuild(-1);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    if (!rejected || pipeline.snapshot().Get() != retained.Get() || pipeline.voxelSize() != 1)
        throw std::runtime_error("Invalid print rebuild replaced visible state");
    pipeline.rebuild(1);
    portable::ChannelSnapshotDesc oldChannel;
    if (NVRHI_FAILED(retained->getChannel(0, oldChannel)) ||
        oldChannel.raw.byteSize != atlas.size() * sizeof(float))
        throw std::runtime_error("Retained print channel");
    std::vector<uint8_t> oldBytes(size_t(oldChannel.raw.byteOffset + oldChannel.raw.byteSize));
    runtime.read(oldChannel.raw.buffer, oldBytes.data(), oldBytes.size());
    if (std::memcmp(oldBytes.data() + size_t(oldChannel.raw.byteOffset), atlas.data(),
                    atlas.size() * sizeof(float)))
        throw std::runtime_error("Print rebuild mutated retained snapshot");
    std::printf("PASS print cube: 56 surface/8 interior voxels, sparse apron and section at origin(%d,%d,%d)\n",
                pipeline.origin().x, pipeline.origin().y, pipeline.origin().z);
}
}  // namespace
void runPrintSelfTests(ethereal::slang::ComputeRuntime& runtime) {
    testCube(runtime, {0, 0, 0, 0});
    testCube(runtime, {-32, 0, -32, 0});
}
}  // namespace gvdb::samples

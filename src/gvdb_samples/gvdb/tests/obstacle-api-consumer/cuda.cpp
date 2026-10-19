// SPDX-License-Identifier: Apache-2.0
// Installed public headers and NVRHI CUDA, without CUDA toolkit discovery.
#include "GVDBNvrhiObstacleApi.h"
#include <nvrhi/cuda.h>
#include <cstdio>
#include <fstream>
#include <vector>
namespace g = gvdb::portable;
std::vector<char> read(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
#define REQUIRE(c)                                                                  \
    do {                                                                            \
        if (!(c)) {                                                                 \
            std::fprintf(stderr, "Installed obstacle failure line %d\n", __LINE__); \
            return 1;                                                               \
        }                                                                           \
    } while (false)
#define CHECK(c) REQUIRE(NVRHI_SUCCEEDED(c))
int main(int argc, char** argv) {
    using namespace nvrhi;
    REQUIRE(argc == 3);
    const auto remap = read(argv[1]), artifact = read(argv[2]);
    cuda::DeviceDesc native;
    native.enableOptiX = false;
    DeviceHandle device = cuda::createDevice(native);
    REQUIRE(device);
    g::VolumeCreateDesc d;
    d.device = device;
    d.configuration.levelCount = 1;
    d.configuration.logDimension = {2};
    d.remapArtifact = remap.data();
    d.remapArtifactBytes = remap.size();
    g::VolumeHandle owner;
    CHECK(g::createVolume(d, &owner));
    g::NodeId root;
    CHECK(owner->allocateNode(0, {-4, -4, -4}, true, root));
    CHECK(owner->setRoot(root));
    CHECK(owner->finishTopology());
    g::GenerationDesc generation;
    generation.placement.minimumBrickCount = {1, 1, 1};
    g::GenerationTicketHandle ticket;
    CHECK(owner->prepareGeneration(generation, &ticket));
    CommandListHandle commands;
    CHECK(device->createCommandList({}, &commands));
    const g::RecordingContext context{device, commands};
    auto submit = [&]() {
        commands->close();
        device->executeCommandList(commands);
        return device->waitForIdle();
    };
    commands->open();
    CHECK(owner->recordAndPublish(context, ticket));
    REQUIRE(submit());
    g::SnapshotHandle snapshot;
    CHECK(owner->getSnapshot(&snapshot));
    g::ObstacleCreateDesc create;
    create.device = device;
    create.artifact = artifact.data();
    create.artifactBytes = artifact.size();
    g::ObstacleHandle op;
    CHECK(g::createObstacle(create, &op));
    uint32_t map = 2;
    g::ObstacleRequestDesc request;
    request.source = snapshot;
    request.obstacle = snapshot;
    request.subcellWidth = 2;
    request.sourceToOutput = &map;
    request.mappingCount = 1;
    request.outputBrickCount = 4;
    g::ObstacleTicketHandle result;
    CHECK(op->prepare(request, {}, &result));
    map = 0;
    snapshot = nullptr;
    owner = nullptr;
    ticket = nullptr;
    commands->open();
    CHECK(op->record(context, result));
    REQUIRE(submit());
    op = nullptr;
    g::BorrowedBytes bytes;
    CHECK(result->finishAfterCallerCompletion(bytes));
    REQUIRE(bytes.byteCount == 32 * 4);
    const auto* values = static_cast<const uint32_t*>(bytes.data);
    for (unsigned i = 0; i < 32; ++i)
        REQUIRE(values[i] == (i >= 16 && i < 24 ? 0 : UINT32_MAX));
    CHECK(result->getValues(bytes));
    device->runGarbageCollection();
    std::puts("PASS installed obstacle copied slots, missing holes, signed root and retained lifetime");
    return 0;
}

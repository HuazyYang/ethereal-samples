// SPDX-License-Identifier: Apache-2.0
// Installed SDK only: no private implementation or native CUDA SDK headers.
#include "GVDBNvrhiGridQueryApi.h"
#include <nvrhi/cuda.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
namespace g = gvdb::portable;
std::vector<char> read(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()};
}
#define REQUIRE(c)                                                                      \
    do {                                                                                \
        if (!(c)) {                                                                     \
            std::fprintf(stderr, "Installed grid query failed at line %d\n", __LINE__); \
            return 1;                                                                   \
        }                                                                               \
    } while (false)
#define CHECK(c) REQUIRE(NVRHI_SUCCEEDED(c))
int main(int argc, char** argv) {
    using namespace nvrhi;
    REQUIRE(argc == 4);
    const auto remap = read(argv[1]), conversion = read(argv[2]), queryCode = read(argv[3]);
    cuda::DeviceDesc native;
    native.enableOptiX = false;
    DeviceHandle device = cuda::createDevice(native);
    REQUIRE(device);
    g::VolumeCreateDesc config;
    config.device = device;
    config.configuration.levelCount = 1;
    config.configuration.logDimension = {2};
    config.remapArtifact = remap.data();
    config.remapArtifactBytes = remap.size();
    g::VolumeHandle owner;
    CHECK(g::createVolume(config, &owner));
    g::NodeId root;
    CHECK(owner->allocateNode(0, {0, 0, 0}, true, root));
    CHECK(owner->setRoot(root));
    CHECK(owner->finishTopology());
    CHECK(owner->setChannel(31, {
                                    g::AtlasFormat::R32_FLOAT, {7, 0, 0, 0}
    }));
    g::GenerationTicketHandle generation;
    g::GenerationDesc gd;
    gd.placement.minimumBrickCount = {1, 1, 1};
    CHECK(owner->prepareGeneration(gd, &generation));
    g::PointOpsCreateDesc pointDesc;
    pointDesc.volume = owner;
    pointDesc.conversionArtifact = conversion.data();
    pointDesc.conversionArtifactBytes = conversion.size();
    g::PointOpsHandle pointFactory;
    CHECK(g::createPointOps(pointDesc, &pointFactory));
    const std::array<g::Float4, 3> positions{
        {{-1, 0, .5f, 17}, {1, 2, 3, 19}, {.25f, .75f, 1, 23}}
    };
    const std::array<uint32_t, 3> colors{
        {0xff010203, 0x80112233, 0x00224466}
    };
    const std::array<g::Float4, 3> velocities{
        {{-3, -7, 2, 17}, {0, -7, 4, 19}, {5, -7, 10, 23}}
    };
    g::PointSetDesc input;
    input.positions.stream = {positions.data(), sizeof(positions), 0, 16, 3, g::PointComponent::Float32, 4};
    input.velocities.stream = {velocities.data(), sizeof(velocities), 0, 16, 3, g::PointComponent::Float32, 4};
    input.colors.stream = {colors.data(), sizeof(colors), 0, 4, 3, g::PointComponent::UInt8, 4};
    g::PointUploadTicketHandle upload;
    CHECK(pointFactory->preparePoints(input, {}, &upload));
    CommandListHandle commands;
    CHECK(device->createCommandList({}, &commands));
    commands->open();
    const g::RecordingContext context{device, commands};
    CHECK(owner->recordAndPublish(context, generation));
    CHECK(pointFactory->recordPoints(context, upload));
    commands->close();
    device->executeCommandList(commands);
    REQUIRE(device->waitForIdle());
    g::PointSetHandle source;
    CHECK(upload->finishAfterCallerCompletion(&source));
    upload = nullptr;
    pointFactory = nullptr;
    g::SnapshotHandle snapshot;
    CHECK(owner->getSnapshot(&snapshot));
    generation = nullptr;
    owner = nullptr;
    g::GridQueryCreateDesc create;
    create.device = device;
    create.artifact = queryCode.data();
    create.artifactBytes = queryCode.size();
    g::GridQueryHandle operation;
    CHECK(g::createGridQuery(create, &operation));
    g::GridQueryRequestDesc request;
    request.snapshot = snapshot;
    request.points = source;
    request.channel = 31;
    std::array<g::GridQueryTicketHandle, 2> tickets;
    for (unsigned i = 0; i < 2; ++i) {
        request.mode = g::GridQueryMode(i);
        CHECK(operation->prepare(request, {}, &tickets[i]));
    }
    source = nullptr;
    snapshot = nullptr;
    commands->open();
    for (auto& t : tickets)
        CHECK(operation->record(context, t));
    commands->close();
    operation = nullptr;
    REQUIRE(device->executeCommandList(commands));
    REQUIRE(device->waitForIdle());
    for (unsigned mode = 0; mode < 2; ++mode) {
        auto& t = tickets[mode];
        g::BorrowedBytes bytes;
        CHECK(t->finishAfterCallerCompletion(bytes));
        REQUIRE(bytes.byteCount == 48);
        const auto* values = static_cast<const g::Float4*>(bytes.data);
        for (unsigned i = 0; i < 3; ++i) {
            const bool valid = i != 0;
            REQUIRE(values[i].x == (valid ? 7.f : 0.f) && values[i].y == (valid && mode ? 7.f : 0.f) &&
                    values[i].z == float(valid) && values[i].w == 0);
        }
        g::BorrowedBufferBinding buffer;
        CHECK(t->getOutputBuffer(buffer));
        REQUIRE(buffer.buffer && buffer.byteSize == 48);
        g::BorrowedBytes again;
        CHECK(t->getValues(again));
        REQUIRE(again.data == bytes.data);
    }
    tickets = {};
    commands = nullptr;
    device->runGarbageCollection();
    std::puts("Installed retained scalar and MAC grid queries PASS");
    return 0;
}

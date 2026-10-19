// SPDX-License-Identifier: Apache-2.0
// Installed SDK only: no private implementation or native CUDA SDK headers.
#include "GVDBNvrhiPointInsertionApi.h"
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
#define REQUIRE(c)                                                                    \
    do {                                                                              \
        if (!(c)) {                                                                   \
            std::fprintf(stderr, "Installed subcells failed at line %d\n", __LINE__); \
            return 1;                                                                 \
        }                                                                             \
    } while (false)
#define CHECK(c) REQUIRE(NVRHI_SUCCEEDED(c))
int main(int argc, char** argv) {
    using namespace nvrhi;
    REQUIRE(argc == 4);
    const auto remap = read(argv[1]), conversion = read(argv[2]), expansion = read(argv[3]);
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
        {{3, 1, 0, 17}, {0, -1, 0, 19}, {5, 0, 0, 23}}
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
    g::SubcellCreateDesc create;
    create.device = device;
    create.artifact = expansion.data();
    create.artifactBytes = expansion.size();
    g::PointInsertionHandle operation;
    CHECK(g::createPointInsertion(create, &operation));
    g::PointInsertionRequestDesc request;
    request.snapshot = snapshot;
    request.points = source;
    request.mode = g::PointInsertionMode::VelocitySupport;
    request.supportOffset = .5f;
    g::SubcellTicketHandle ticket;
    CHECK(operation->prepare(request, {}, &ticket));
    source = nullptr;
    snapshot = nullptr;
    std::vector<uint32_t> expected;
    struct Range {
        uint32_t offset, count;
    };
    std::array<Range, 1> ranges{};
    for (unsigned c = 0; c < 1; ++c) {
        ranges[c].offset = uint32_t(expected.size());
        const float origin[] = {float((c % 2) * 2), float(((c / 2) % 2) * 2), float((c / 4) * 2)};
        for (unsigned i = 0; i < 3; ++i) {
            const float pos[] = {positions[i].x + velocities[i].x * .5f, positions[i].y + velocities[i].y * .5f,
                                 positions[i].z + velocities[i].z * .5f};
            bool found = true;
            for (unsigned axis = 0; axis < 3; ++axis)
                found &= origin[axis] <= pos[axis] && origin[axis] + 4 > pos[axis];
            if (found) {
                expected.push_back(i);
                ++ranges[c].count;
            }
        }
    }
    g::BorrowedBytes cells, ordinals;
    CHECK(ticket->getCellRanges(cells));
    CHECK(ticket->getSourceOrdinals(ordinals));
    REQUIRE(cells.byteCount == 8 && !std::memcmp(cells.data, ranges.data(), 8));
    REQUIRE(ordinals.byteCount == expected.size() * 4 &&
            !std::memcmp(ordinals.data, expected.data(), ordinals.byteCount));
    g::BorrowedBytes locations;
    CHECK(ticket->getCellLocations(locations));
    REQUIRE(locations.byteCount == 16);
    const auto* location = static_cast<const g::SubcellLocation*>(locations.data);
    for (unsigned i = 0; i < 1; ++i)
        REQUIRE(location[i].leafIndex == 0 && location[i].origin.x == int(i % 2) * 2 &&
                location[i].origin.y == int((i / 2) % 2) * 2 && location[i].origin.z == int(i / 4) * 2);
    commands->open();
    CHECK(operation->record(context, ticket));
    commands->close();
    operation = nullptr;
    REQUIRE(device->executeCommandList(commands));
    REQUIRE(device->waitForIdle());
    g::PointSetHandle result;
    CHECK(ticket->finishAfterCallerCompletion(&result));
    ticket = nullptr;
    REQUIRE(result->getDesc().count == expected.size());
    for (unsigned a = 0; a < 3; ++a) {
        g::BorrowedBytes values;
        CHECK(result->getData(g::PointAttribute(a), values));
        const size_t stride = a == 2 ? 4 : 16;
        REQUIRE(values.byteCount == expected.size() * stride);
        for (size_t i = 0; i < expected.size(); ++i) {
            const void* sourceValue = a == 0   ? static_cast<const void*>(&positions[expected[i]])
                                      : a == 1 ? static_cast<const void*>(&velocities[expected[i]])
                                               : static_cast<const void*>(&colors[expected[i]]);
            REQUIRE(!std::memcmp(static_cast<const uint8_t*>(values.data) + i * stride, sourceValue, stride));
        }
    }
    result = nullptr;
    commands = nullptr;
    device->runGarbageCollection();
    std::puts("Installed retained insertion: stable ranges/ordinals and exact GPU attributes PASS");
    return 0;
}

// SPDX-License-Identifier: Apache-2.0
// Installed SDK only: no private implementation or native CUDA SDK headers.
#include "GVDBNvrhiDepositApi.h"
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
            std::fprintf(stderr, "Installed deposition failed at line %d\n", __LINE__); \
            return 1;                                                                   \
        }                                                                               \
    } while (false)
#define CHECK(c) REQUIRE(NVRHI_SUCCEEDED(c))
int main(int argc, char** argv) {
    using namespace nvrhi;
    REQUIRE(argc == 4);
    const auto remap = read(argv[1]), conversion = read(argv[2]), artifact = read(argv[3]);
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
    g::PointOpsCreateDesc pointDesc;
    pointDesc.volume = owner;
    pointDesc.conversionArtifact = conversion.data();
    pointDesc.conversionArtifactBytes = conversion.size();
    g::PointOpsHandle pointFactory;
    CHECK(g::createPointOps(pointDesc, &pointFactory));
    const std::array<g::Float4, 2> positions{
        {{1, 1, 1, 17}, {1, 1, 1, 19}}
    };
    const std::array<uint32_t, 2> colors{
        {0x40201000, 0x80a06020}
    };
    const std::array<g::Float4, 2> directions{};
    g::PointSetDesc input;
    input.positions.stream = {positions.data(), sizeof(positions), 0, 16, 2, g::PointComponent::Float32, 4};
    input.colors.stream = {colors.data(), sizeof(colors), 0, 4, 2, g::PointComponent::UInt8, 4};
    input.velocities.stream = {directions.data(), sizeof(directions), 0, 16, 2, g::PointComponent::Float32, 4};
    g::PointUploadTicketHandle upload;
    CHECK(pointFactory->preparePoints(input, {}, &upload));
    CommandListHandle commands;
    CHECK(device->createCommandList({}, &commands));
    commands->open();
    const g::RecordingContext context{device, commands};
    CHECK(pointFactory->recordPoints(context, upload));
    commands->close();
    device->executeCommandList(commands);
    REQUIRE(device->waitForIdle());
    g::PointSetHandle source;
    CHECK(upload->finishAfterCallerCompletion(&source));

    upload = nullptr;
    pointFactory = nullptr;
    g::NodeId root;
    CHECK(owner->allocateNode(0, {0, 0, 0}, true, root));
    CHECK(owner->setRoot(root));
    CHECK(owner->finishTopology());
    CHECK(owner->setChannel(0, {g::AtlasFormat::R32_FLOAT, {}}));
    CHECK(owner->setChannel(2, {g::AtlasFormat::RGBA8_UINT, {}}));
    CHECK(owner->setChannel(3, {g::AtlasFormat::R8_UINT, {}}));
    auto submit = [&]() {
        commands->close();
        device->executeCommandList(commands);
        return device->waitForIdle();
    };
    for (unsigned mode = 0; mode < 3; ++mode) {
        g::GenerationDesc generation;
        generation.placement.minimumBrickCount = {1, 1, 1};
        generation.placement.preserve = 0;
        g::GenerationTicketHandle ticket;
        CHECK(owner->prepareGeneration(generation, &ticket));
        commands->open();
        CHECK(owner->recordAndPublish(context, ticket));
        REQUIRE(submit());
        g::DepositCreateDesc create;
        create.volume = owner;
        create.artifact = artifact.data();
        create.artifactBytes = artifact.size();
        g::DepositHandle operation;
        CHECK(g::createDeposit(create, &operation));
        g::DepositRequestDesc request;
        request.points = source;
        request.kind = g::DepositKind(mode);
        if (mode < 2) {
            request.color = mode ? g::DepositColor::AverageRGB : g::DepositColor::LastOrdinal;
            request.colorChannel = 2;
        } else
            request.materialChannel = 3;
        CHECK(operation->prepare(request, {}, &ticket));
        operation = nullptr;
        commands->open();
        CHECK(owner->recordAndPublish(context, ticket));
        REQUIRE(submit());
        g::SnapshotHandle snapshot;
        CHECK(owner->getSnapshot(&snapshot));
        g::ReadbackHandle output;
        CHECK(owner->prepareReadback(snapshot, 13, &output));
        commands->open();
        CHECK(owner->recordReadback(context, output));
        REQUIRE(submit());
        CHECK(output->finishAfterCallerCompletion());
        g::BorrowedBytes scalar, color, material;
        CHECK(output->getChannelData(0, scalar));
        CHECK(output->getChannelData(2, color));
        CHECK(output->getChannelData(3, material));
        REQUIRE(scalar.byteCount == 216 * 4 && color.byteCount == 216 * 4 && material.byteCount == 216);
        for (unsigned z = 0; z < 6; ++z)
            for (unsigned y = 0; y < 6; ++y)
                for (unsigned x = 0; x < 6; ++x) {
                    unsigned i = (z * 6 + y) * 6 + x;
                    float value;
                    uint32_t rgba;
                    std::memcpy(&value, static_cast<const uint8_t*>(scalar.data) + i * 4, 4);
                    std::memcpy(&rgba, static_cast<const uint8_t*>(color.data) + i * 4, 4);
                    bool center = x == 2 && y == 2 && z == 2;
                    int dx = int(x) - 2, dz = int(z) - 2;
                    float expected = center                                                          ? 2.f
                                     : mode == 2 && y == 2 && std::abs(dx) == 1 && std::abs(dz) == 1 ? -2.f
                                                                                                     : 0.f;
                    REQUIRE(value == expected);
                    REQUIRE(rgba == (center && mode < 2 ? (mode ? 0xff603810u : 0x80a06020u) : 0));
                    REQUIRE(static_cast<const uint8_t*>(material.data)[i] ==
                            (mode == 2 && y == 2 && x >= 1 && x <= 3 && z >= 1 && z <= 3 ? 1 : 0));
                }
    }
    std::puts("PASS installed deposition: central/axis/XZ scalar, exact colors/material, released factory");
    return 0;
}

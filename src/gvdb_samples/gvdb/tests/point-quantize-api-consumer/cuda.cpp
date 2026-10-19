// SPDX-License-Identifier: Apache-2.0
// Installed SDK only: no private implementation or native CUDA SDK headers.
#include "GVDBNvrhiPointQuantizeApi.h"
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
#define REQUIRE(c)                                                                        \
    do {                                                                                  \
        if (!(c)) {                                                                       \
            std::fprintf(stderr, "Installed quantization failed at line %d\n", __LINE__); \
            return 1;                                                                     \
        }                                                                                 \
    } while (false)
#define CHECK(c) REQUIRE(NVRHI_SUCCEEDED(c))
int main(int argc, char** argv) {
    using namespace nvrhi;
    REQUIRE(argc == 5);
    const auto remap = read(argv[1]), conversion = read(argv[2]), encode = read(argv[3]),
               decode = read(argv[4]);
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
    const std::array<g::Float4, 3> positions{
        {{-1, 0, .5f, 17}, {1, 2, 3, 19}, {.25f, .75f, 1, 23}}
    };
    const std::array<uint32_t, 3> colors{
        {0xff010203, 0x80112233, 0x00224466}
    };
    g::PointSetDesc input;
    input.positions.stream = {positions.data(), sizeof(positions), 0, 16, 3, g::PointComponent::Float32, 4};
    input.colors.stream = {colors.data(), sizeof(colors), 0, 4, 3, g::PointComponent::UInt8, 4};
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
    owner = nullptr;
    g::PointQuantizeCreateDesc create;
    create.device = device;
    create.encodeArtifact = encode.data();
    create.encodeArtifactBytes = encode.size();
    create.decodeArtifact = decode.data();
    create.decodeArtifactBytes = decode.size();
    g::PointQuantizeHandle operation;
    CHECK(g::createPointQuantize(create, &operation));
    g::PointQuantizeRequestDesc request;
    request.points = source;
    g::PointQuantizeTicketHandle ticket;
    CHECK(operation->prepare(request, {}, &ticket));
    source = nullptr;
    commands->open();
    CHECK(operation->record(context, ticket));
    operation = nullptr;
    commands->close();
    device->executeCommandList(commands);
    REQUIRE(device->waitForIdle());
    g::PointSetHandle result;
    CHECK(ticket->finishAfterCallerCompletion(&result));
    g::BorrowedBytes codes, values, actualColors;
    CHECK(ticket->getCodes(codes));
    CHECK(result->getData(g::PointAttribute::Position, values));
    CHECK(result->getData(g::PointAttribute::Color, actualColors));
    REQUIRE(codes.byteCount == 18 && values.byteCount == 48 && actualColors.byteCount == 12);
    REQUIRE(!std::memcmp(colors.data(), actualColors.data, 12));
    const uint16_t expected[] = {0, 0, 32767, 65535, 65535, 65535, 16383, 49151, 65535};
    for (size_t i = 0; i < 9; ++i) {
        uint16_t code;
        float value;
        std::memcpy(&code, static_cast<const uint8_t*>(codes.data) + i * 2, 2);
        std::memcpy(&value, static_cast<const uint8_t*>(values.data) + (i / 3) * 16 + (i % 3) * 4, 4);
        REQUIRE(std::abs(int(code) - int(expected[i])) <= 1);
        REQUIRE(std::abs(double(value) - double(code) / 65535.) <= 2e-6);
    }
    for (size_t i = 0; i < 3; ++i)
        REQUIRE(!std::memcmp(&positions[i].w, static_cast<const uint8_t*>(values.data) + i * 16 + 12, 4));
    ticket = nullptr;  // Result storage and copied attributes survive ticket destruction.
    g::BorrowedPointStream stream;
    CHECK(result->getStream(g::PointAttribute::Position, stream));
    REQUIRE(stream.buffer && stream.components == 4 && stream.count == 3);
    result = nullptr;
    commands = nullptr;
    device->runGarbageCollection();
    std::puts("Installed quantization: codes, decoded points, attributes and released factories PASS");
    return 0;
}

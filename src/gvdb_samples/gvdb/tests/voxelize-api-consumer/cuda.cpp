// SPDX-License-Identifier: Apache-2.0
// Public installed headers only; no source-tree include directories.
#include <GVDBNvrhiVoxelizeApi.h>
#include <nvrhi/cuda.h>
#include <nvrhi/core/foundation.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
using namespace nvrhi;
namespace g = gvdb::portable;
void require(bool value, const char* label) {
    if (!value)
        throw std::runtime_error(label);
}
void check(FRESULT result) { require(NVRHI_SUCCEEDED(result), "public API call failed"); }
class Messages : public ObjectImpl<IMessageCallback> {
 public:
    unsigned errors = 0;
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity severity, const char* text) noexcept override {
        if (severity >= MessageSeverity::Warning)
            ++errors;
        std::fprintf(stderr, "%s\n", text);
    }
};
int main(int argc, char** argv) {
    try {
        require(argc == 2, "artifact directory required");
        auto callback = MAKE_RC_OBJ_PTR(Messages);
        cuda::DeviceDesc dd;
        dd.enableOptiX = false;
        dd.errorCB = callback;
        auto device = cuda::createDevice(dd);
        require(bool(device), "CUDA device");
        CommandListHandle commands;
        check(device->createCommandList({}, &commands));
        auto submit = [&] {
            commands->close();
            require(device->executeCommandList(commands) != 0, "submit");
            require(device->waitForIdle(), "completion");
            device->runGarbageCollection();
        };
        std::array<std::vector<char>, 4> binaries;
        const char* entries[] = {"clearTriangleBins", "countTriangleBins", "scatterTriangleBins",
                                 "solidVoxelize"};
        g::VoxelizeCreateDesc create;
        create.device = device;
        for (unsigned i = 0; i < 4; ++i) {
            std::ifstream f(
                std::filesystem::path(argv[1]) / (std::string("GVDBMeshVoxelize_") + entries[i] + ".bin"),
                std::ios::binary);
            require(bool(f), "artifact");
            binaries[i] = {(std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()};
            create.artifacts[i] = {binaries[i].data(), binaries[i].size()};
        }
        for (auto format :
             {g::MeshVoxelFormat::R8_UINT, g::MeshVoxelFormat::R32_FLOAT, g::MeshVoxelFormat::R32_SINT}) {
            g::VoxelizeHandle op;
            check(g::createVoxelize(create, &op));
            std::vector<g::Float4> vertices = {
                {1.25f, 1.25f, 1.25f, 1},
                {4.75f, 1.25f, 1.25f, 1},
                {4.75f, 4.75f, 1.25f, 1},
                {1.25f, 4.75f, 1.25f, 1},
                {1.25f, 1.25f, 4.75f, 1},
                {4.75f, 1.25f, 4.75f, 1},
                {4.75f, 4.75f, 4.75f, 1},
                {1.25f, 4.75f, 4.75f, 1}
            };
            std::vector<uint32_t> indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                             3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
            g::VoxelizeRequestDesc request;
            request.vertices = vertices.data();
            request.vertexCount = vertices.size();
            request.indices = indices.data();
            request.indexCount = indices.size();
            request.resolution = {6, 6, 6};
            request.boundsMaximum = {6, 6, 6, 0};
            request.values = {9, 3, 0, 0};
            request.format = format;
            g::VoxelizeTicketHandle ticket;
            check(op->prepare(request, {}, &ticket));
            vertices.clear();
            indices.clear();
            commands->open();
            check(op->recordCounts({device, commands}, ticket));
            submit();
            check(ticket->finishCountsAfterCallerCompletion());
            commands->open();
            check(op->recordVoxelization({device, commands}, ticket));
            op = nullptr;
            submit();
            g::BorrowedBytes bytes;
            check(ticket->finishAfterCallerCompletion(bytes));
            const unsigned stride = format == g::MeshVoxelFormat::R8_UINT ? 1 : 4;
            require(bytes.byteCount == 216 * stride, "byte count");
            for (unsigned z = 0; z < 6; ++z)
                for (unsigned y = 0; y < 6; ++y)
                    for (unsigned x = 0; x < 6; ++x) {
                        const unsigned index = (z * 6 + y) * 6 + x;
                        float expected = 0;
                        if (x >= 1 && x <= 4 && y >= 1 && y <= 4 && z >= 1 && z <= 4)
                            expected = (x == 1 || x == 4 || y == 1 || y == 4 || z == 1 || z == 4) ? 9.f : 3.f;
                        const auto* data = static_cast<const uint8_t*>(bytes.data) + index * stride;
                        float actual;
                        if (stride == 1)
                            actual = *data;
                        else if (format == g::MeshVoxelFormat::R32_FLOAT)
                            std::memcpy(&actual, data, 4);
                        else {
                            int32_t v;
                            std::memcpy(&v, data, 4);
                            actual = float(v);
                        }
                        require(actual == expected, "closed box golden");
                    }
            g::BorrowedBufferBinding output;
            check(ticket->getOutputBuffer(output));
            require(output.buffer && output.byteSize == bytes.byteCount, "retained GPU output");
        }
        require(callback->errors == 0, "validation messages");
        std::puts("Installed public voxelizer formats, phases, copied inputs and release lifetime PASS");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}

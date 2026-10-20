// SPDX-License-Identifier: Apache-2.0
#include "../RayQueryApiFixture.h"
#include <nvrhi/cuda.h>
#include <fstream>
#include <cstdio>
int main(int argc, char** argv) {
    try {
        ray_query_fixture::require(argc == 3, "two artifact paths required");
        auto read = [](const char* path) {
            std::ifstream file(path, std::ios::binary);
            ray_query_fixture::require(bool(file), "open ray consumer artifact");
            return std::vector<char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        };
        nvrhi::cuda::DeviceDesc desc;
        desc.enableOptiX = false;
        auto device = nvrhi::cuda::createDevice(desc);
        ray_query_fixture::require(bool(device), "installed CUDA device");
        ray_query_fixture::run(device, read(argv[1]), read(argv[2]));
        std::puts("PASS installed public ray records, exact bytes and retained lifetime");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}

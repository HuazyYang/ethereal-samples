// SPDX-License-Identifier: Apache-2.0
#include "../TopologyEditApiFixture.h"
#include <nvrhi/cuda.h>
#include <fstream>
#include <cstdio>
int main(int argc, char** argv) {
    try {
        topology_edit_fixture::require(argc == 3, "two artifact paths required");
        auto read = [](const char* path) {
            std::ifstream file(path, std::ios::binary);
            topology_edit_fixture::require(bool(file), "open topology artifact");
            return std::vector<char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        };
        nvrhi::cuda::DeviceDesc desc;
        desc.enableOptiX = false;
        auto device = nvrhi::cuda::createDevice(desc);
        topology_edit_fixture::require(bool(device), "installed CUDA device");
        topology_edit_fixture::run(device, read(argv[1]), read(argv[2]));
        std::puts("PASS installed private GPU topology editing and validated publication");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}

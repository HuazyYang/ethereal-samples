// SPDX-License-Identifier: Apache-2.0
#include "../TopologyActivityApiFixture.h"
#include <nvrhi/cuda.h>
#include <fstream>
#include <cstdio>
#include <filesystem>
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
        const char* entries[] = {"applyLeafMarkers", "delinkLeafBricks", "delinkInternalBricks",
                                 "initializeChildLists", "linkBricks"};
        std::array<std::vector<char>, 5> artifacts;
        for (size_t i = 0; i < artifacts.size(); ++i) {
            auto path = std::filesystem::path(argv[2]).parent_path() /
                        (std::string("GVDBTopology_") + entries[i] + ".bin");
            artifacts[i] = read(path.string().c_str());
        }
        topology_edit_fixture::runActivity(device, read(argv[1]), artifacts);
        std::puts("PASS installed private GPU topology editing and validated publication");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}

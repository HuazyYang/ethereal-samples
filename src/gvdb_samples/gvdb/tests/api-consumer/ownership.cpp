// Installed C++ cores remain optional alternatives to the queryable API.
#include "GVDBNvrhiVolume.h"
#include <cstdio>
int main() {
    using namespace gvdb::portable;
    HostVolume host;
    LevelConfig config;
    config.levelCount = 2; config.logDimension = {2,1};
    if (NVRHI_FAILED(host.configure(config))) return 1;
    NodeId a, b;
    if (NVRHI_FAILED(host.activate(0,0,0,a)) || NVRHI_FAILED(host.activate(4,0,0,b))) return 2;
    if (a == b || NVRHI_FAILED(host.finishTopology()) || host.activeLeafCount() != 2) return 3;
    std::unique_ptr<NvrhiVolume> volume;
    if (NvrhiVolume::create({},volume) != nvrhi::FE_INVALID_ARGS || volume) return 4;
    std::puts("Installed CPU/GPU ownership libraries link and validate inputs PASS");
    return 0;
}

// SPDX-License-Identifier: Apache-2.0
// CPU-only factory ABI and descriptor layout fixture, no backend/SDK/device.
#include "GVDBNvrhiPointApi.h"
#include <cstdio>
int main() {
    namespace g = gvdb::portable;
    using namespace nvrhi;
    static_assert(offsetof(g::HostPointStreamDesc, data) == 0 && offsetof(g::HostPointStreamDesc, count) == 32);
    static_assert(offsetof(g::PointSetDesc, positions) == 8 && offsetof(g::PointSetDesc, velocities) == 120 &&
                  offsetof(g::PointSetDesc, colors) == 232);
    static_assert(offsetof(g::PointOpsCreateDesc, volume) == 8 &&
                  offsetof(g::PointOpsCreateDesc, conversionArtifactBytes) == 24);
    g::PointOpsCreateDesc desc;
    auto* sentinel = reinterpret_cast<g::IGVDBPointOps*>(uintptr_t(1));
    auto* output = sentinel;
    if (g::gvdbCreateNvrhiPointOps(g::c_GVDBNvrhiApiVersion + 1, &desc, &output) != FE_UNSUPPORTED || output)
        return 1;
    desc.structSize--;
    output = sentinel;
    if (g::createPointOps(desc, &output) != FE_INVALID_ARGS || output)
        return 2;
    desc = {};
    desc.reserved = 1;
    output = sentinel;
    if (g::createPointOps(desc, &output) != FE_INVALID_ARGS || output)
        return 3;
    desc = {};
    output = sentinel;
    if (g::createPointOps(desc, &output) != FE_INVALID_ARGS || output)
        return 4;
    output = sentinel;
    if (g::gvdbCreateNvrhiPointOps(g::c_GVDBNvrhiApiVersion, nullptr, &output) != FE_INVALID_ARGS || output)
        return 5;
    if (g::createPointOps(desc, nullptr) != FE_INVALID_ARGS)
        return 6;
    std::puts("GVDB public point ABI: layout/version/size/reserved/null output rules PASS");
    return 0;
}

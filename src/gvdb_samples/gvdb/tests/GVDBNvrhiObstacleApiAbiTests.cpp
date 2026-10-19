// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiObstacleApi.h"
#include <cstdio>
int main() {
    namespace g = gvdb::portable;
    using namespace nvrhi;
    g::ObstacleRequestDesc request;
    g::ObstaclePreparationBudget budget;
    g::ObstacleCreateDesc desc;
    if (request.structSize != 48 || request.subcellWidth != 1 || request.source || request.obstacle ||
        request.sourceToOutput || request.mappingCount || request.outputBrickCount || request.reserved[0] ||
        request.reserved[1])
        return 1;
    if (budget.maxHostBytes != UINT64_MAX || budget.maxAllocatedBytes != UINT64_MAX ||
        budget.maxBuffers != UINT32_MAX || budget.reserved)
        return 2;
    auto* out = reinterpret_cast<g::IGVDBObstacle*>(uintptr_t(1));
    if (g::gvdbCreateNvrhiObstacle(g::c_GVDBNvrhiApiVersion + 1, &desc, &out) != FE_UNSUPPORTED || out)
        return 3;
    if (g::createObstacle(desc, nullptr) != FE_INVALID_ARGS)
        return 4;
    for (unsigned i = 0; i < 4; ++i) {
        desc = {};
        if (i == 0)
            --desc.structSize;
        if (i == 1)
            desc.reserved = 1;
        out = reinterpret_cast<g::IGVDBObstacle*>(uintptr_t(1));
        if (g::gvdbCreateNvrhiObstacle(g::c_GVDBNvrhiApiVersion, i == 3 ? nullptr : &desc, &out) !=
                FE_INVALID_ARGS ||
            out)
            return 5;
    }
    std::puts("PASS obstacle ABI defaults/version/size/reserved/null outputs");
    return 0;
}

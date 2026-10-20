// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiRayQueryApi.h"
#include <cstdio>
int main() {
    namespace g = gvdb::portable;
    using namespace nvrhi;
    g::RayQueryRequestDesc request;
    g::RayQueryPreparationBudget budget;
    g::RayQueryCreateDesc desc;
    if (request.structSize != 64 || request.channel || request.snapshot || request.rays || request.count ||
        request.step != .25f || request.reserved || request.reserved2)
        return 1;
    if (budget.maxHostBytes != UINT64_MAX || budget.maxAllocatedBytes != UINT64_MAX ||
        budget.maxBuffers != UINT32_MAX || budget.reserved)
        return 2;
    auto* out = reinterpret_cast<g::IGVDBRayQuery*>(uintptr_t(1));
    if (g::gvdbCreateNvrhiRayQuery(g::c_GVDBNvrhiApiVersion + 1, &desc, &out) != FE_UNSUPPORTED || out)
        return 3;
    if (g::createRayQuery(desc, nullptr) != FE_INVALID_ARGS)
        return 4;
    for (unsigned i = 0; i < 4; ++i) {
        desc = {};
        if (i == 0)
            --desc.structSize;
        if (i == 1)
            desc.reserved = 1;
        out = reinterpret_cast<g::IGVDBRayQuery*>(uintptr_t(1));
        if (g::gvdbCreateNvrhiRayQuery(g::c_GVDBNvrhiApiVersion, i == 3 ? nullptr : &desc, &out) !=
                FE_INVALID_ARGS ||
            out)
            return 5;
    }
    std::puts("PASS ray query ABI defaults/version/size/reserved/null outputs");
    return 0;
}

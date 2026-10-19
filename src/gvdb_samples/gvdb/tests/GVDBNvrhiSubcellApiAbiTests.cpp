// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiSubcellApi.h"
#include <cstdio>
int main() {
    using namespace nvrhi;
    namespace g = gvdb::portable;
    g::SubcellCreateDesc desc;
    g::SubcellRequestDesc request;
    g::SubcellPreparationBudget budget;
    if (request.structSize != 48 || request.subcellWidth != 1 || request.snapshot || request.points ||
        request.radius || request.reserved)
        return 1;
    if (budget.maxCells != UINT64_MAX || budget.maxCandidateVisits != UINT64_MAX ||
        budget.maxPointPairs != UINT64_MAX || budget.maxBuffers != UINT32_MAX || budget.reserved)
        return 2;
    auto* sentinel = reinterpret_cast<g::IGVDBSubcells*>(uintptr_t(1));
    auto* output = sentinel;
    if (g::gvdbCreateNvrhiSubcells(g::c_GVDBNvrhiApiVersion + 1, &desc, &output) != FE_UNSUPPORTED || output)
        return 3;
    if (g::createSubcells(desc, nullptr) != FE_INVALID_ARGS)
        return 4;
    output = sentinel;
    if (g::gvdbCreateNvrhiSubcells(g::c_GVDBNvrhiApiVersion, nullptr, &output) != FE_INVALID_ARGS || output)
        return 5;
    output = sentinel;
    if (g::createSubcells(desc, &output) != FE_INVALID_ARGS || output)
        return 6;
    desc.structSize--;
    output = sentinel;
    if (g::createSubcells(desc, &output) != FE_INVALID_ARGS || output)
        return 7;
    std::puts("Subcell API descriptor/version/output contract passed");
    return 0;
}

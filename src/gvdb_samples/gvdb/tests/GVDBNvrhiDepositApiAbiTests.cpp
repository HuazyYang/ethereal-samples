// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiDepositApi.h"
#include <cstdio>
int main() {
    namespace g = gvdb::portable;
    using namespace nvrhi;
    g::DepositRequestDesc request;
    g::DepositPreparationBudget budget;
    g::DepositCreateDesc desc;
    static_assert(uint32_t(g::DepositKind::SupportXZ) == 2 && uint32_t(g::DepositColor::AverageRGB) == 2);
    if (request.structSize != 64 || request.reserved || request.reserved2 || request.points ||
        request.channel || request.radius != 1 || request.directionOffset ||
        request.colorChannel != UINT32_MAX || request.materialChannel != UINT32_MAX)
        return 1;
    if (budget.maxPoints != UINT64_MAX || budget.maxContributions != UINT64_MAX ||
        budget.maxHostBytes != UINT64_MAX || budget.maxAllocatedBytes != UINT64_MAX ||
        budget.maxBuffers != UINT32_MAX || budget.reserved)
        return 2;
    auto* out = reinterpret_cast<g::IGVDBDeposit*>(uintptr_t(1));
    if (g::gvdbCreateNvrhiDeposit(g::c_GVDBNvrhiApiVersion + 1, &desc, &out) != FE_UNSUPPORTED || out)
        return 3;
    if (g::createDeposit(desc, nullptr) != FE_INVALID_ARGS)
        return 4;
    for (unsigned i = 0; i < 4; ++i) {
        desc = {};
        if (i == 0)
            --desc.structSize;
        if (i == 1)
            desc.reserved = 1;
        out = reinterpret_cast<g::IGVDBDeposit*>(uintptr_t(1));
        if (g::gvdbCreateNvrhiDeposit(g::c_GVDBNvrhiApiVersion, i == 3 ? nullptr : &desc, &out) !=
                FE_INVALID_ARGS ||
            out)
            return 5;
    }
    std::puts("PASS deposition ABI defaults/version/size/reserved/null outputs");
    return 0;
}

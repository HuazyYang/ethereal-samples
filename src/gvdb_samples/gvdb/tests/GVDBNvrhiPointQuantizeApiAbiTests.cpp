// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiPointQuantizeApi.h"
#include <cstdio>
int main() {
    namespace g = gvdb::portable;
    using namespace nvrhi;
    g::PointQuantizeCreateDesc desc;
    g::PointQuantizeRequestDesc request;
    g::PointQuantizePreparationBudget budget;
    if (request.structSize != 56 || request.reserved || request.reserved2 || request.points ||
        request.bounds != g::PointQuantizationBounds::Explicit || request.minimum.x || request.range.x != 1)
        return 1;
    if (budget.maxPoints != UINT64_MAX || budget.maxAllocatedBytes != UINT64_MAX ||
        budget.maxHostBytes != UINT64_MAX || budget.maxBuffers != UINT32_MAX || budget.reserved)
        return 2;
    auto* sentinel = reinterpret_cast<g::IGVDBPointQuantize*>(uintptr_t(1));
    auto* output = sentinel;
    if (g::gvdbCreateNvrhiPointQuantize(g::c_GVDBNvrhiApiVersion + 1, &desc, &output) != FE_UNSUPPORTED ||
        output)
        return 3;
    desc.structSize--;
    output = sentinel;
    if (g::createPointQuantize(desc, &output) != FE_INVALID_ARGS || output)
        return 4;
    desc = {};
    desc.reserved = 1;
    output = sentinel;
    if (g::createPointQuantize(desc, &output) != FE_INVALID_ARGS || output)
        return 5;
    desc = {};
    output = sentinel;
    if (g::createPointQuantize(desc, &output) != FE_INVALID_ARGS || output)
        return 6;
    output = sentinel;
    if (g::gvdbCreateNvrhiPointQuantize(g::c_GVDBNvrhiApiVersion, nullptr, &output) != FE_INVALID_ARGS ||
        output)
        return 7;
    if (g::createPointQuantize(desc, nullptr) != FE_INVALID_ARGS)
        return 8;
    output = sentinel;
    if (g::gvdbCreateNvrhiVelocityQuantize(g::c_GVDBNvrhiApiVersion + 1, &desc, &output) != FE_UNSUPPORTED ||
        output)
        return 9;
    output = sentinel;
    if (g::createVelocityQuantize(desc, &output) != FE_INVALID_ARGS || output)
        return 10;
    if (g::createVelocityQuantize(desc, nullptr) != FE_INVALID_ARGS)
        return 11;
    std::puts("GVDB point quantize ABI defaults/version/size/reserved/null outputs PASS");
    return 0;
}

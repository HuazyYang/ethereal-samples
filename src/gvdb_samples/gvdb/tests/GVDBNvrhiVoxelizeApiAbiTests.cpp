// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiVoxelizeApi.h"
#include <cstdio>
int main() {
    namespace g = gvdb::portable;
    using namespace nvrhi;
    g::VoxelizeCreateDesc desc;
    g::VoxelizeRequestDesc request;
    g::VoxelizePreparationBudget budget;
    if (request.structSize != 160 || request.reserved || request.reserved2[0] || request.reserved2[1] ||
        request.vertices || request.indices || request.vertexCount || request.indexCount)
        return 1;
    if (budget.maxInputBytes != UINT64_MAX || budget.maxTriangleBinVisits != UINT64_MAX ||
        budget.maxVoxelTriangleVisits != UINT64_MAX || budget.maxBuffers != UINT32_MAX || budget.reserved)
        return 2;
    auto* sentinel = reinterpret_cast<g::IGVDBVoxelize*>(uintptr_t(1));
    auto* output = sentinel;
    if (g::gvdbCreateNvrhiVoxelize(g::c_GVDBNvrhiApiVersion + 1, &desc, &output) != FE_UNSUPPORTED || output)
        return 3;
    desc.structSize--;
    output = sentinel;
    if (g::createVoxelize(desc, &output) != FE_INVALID_ARGS || output)
        return 4;
    desc = {};
    desc.reserved = 1;
    output = sentinel;
    if (g::createVoxelize(desc, &output) != FE_INVALID_ARGS || output)
        return 5;
    desc = {};
    output = sentinel;
    if (g::createVoxelize(desc, &output) != FE_INVALID_ARGS || output)
        return 6;
    output = sentinel;
    if (g::gvdbCreateNvrhiVoxelize(g::c_GVDBNvrhiApiVersion, nullptr, &output) != FE_INVALID_ARGS || output)
        return 7;
    if (g::createVoxelize(desc, nullptr) != FE_INVALID_ARGS)
        return 8;
    std::puts("GVDB voxelize ABI defaults/version/size/reserved/null outputs PASS");
    return 0;
}

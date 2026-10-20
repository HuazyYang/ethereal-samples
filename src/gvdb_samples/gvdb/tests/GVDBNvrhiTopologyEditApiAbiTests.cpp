// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiTopologyEditApi.h"
#include <cstdio>
int main() {
    namespace g = gvdb::portable;
    using namespace nvrhi;
    g::TopologyEditDesc request;
    g::TopologyEditCreateDesc desc;
    if (request.structSize != 64 || request.reserved || request.preserveMetadata != 1 ||
        request.maxReadbackBytes != UINT64_MAX || request.maxAllocatedBytes != UINT64_MAX ||
        request.maxPublicationBytes != UINT64_MAX || request.maxBuffers != UINT32_MAX ||
        request.maxPublicationBuffers != UINT32_MAX)
        return 1;
    auto* out = reinterpret_cast<g::IGVDBTopologyEdit*>(uintptr_t(1));
    if (g::gvdbCreateNvrhiTopologyEdit(g::c_GVDBNvrhiApiVersion + 1, &desc, &out) != FE_UNSUPPORTED || out)
        return 2;
    if (g::createTopologyEdit(desc, nullptr) != FE_INVALID_ARGS)
        return 3;
    for (unsigned i = 0; i < 4; ++i) {
        desc = {};
        if (i == 0)
            --desc.structSize;
        if (i == 1)
            desc.reserved = 1;
        out = reinterpret_cast<g::IGVDBTopologyEdit*>(uintptr_t(1));
        if (g::gvdbCreateNvrhiTopologyEdit(g::c_GVDBNvrhiApiVersion, i == 3 ? nullptr : &desc, &out) !=
                FE_INVALID_ARGS ||
            out)
            return 4;
    }
    g::TopologyActivityCreateDesc activity;
    g::TopologyActivityDesc activityRequest;
    if (activity.structSize != 96 || activity.reserved || activity.device || activityRequest.structSize != 48 ||
        activityRequest.reserved || activityRequest.reserved2 ||
        activityRequest.maxAllocatedBytes != UINT64_MAX || activityRequest.maxBuffers != UINT32_MAX)
        return 5;
    auto* activityOut = reinterpret_cast<g::IGVDBTopologyActivity*>(uintptr_t(1));
    if (g::gvdbCreateNvrhiTopologyActivity(g::c_GVDBNvrhiApiVersion + 1, &activity, &activityOut) !=
            FE_UNSUPPORTED ||
        activityOut)
        return 6;
    activityOut = reinterpret_cast<g::IGVDBTopologyActivity*>(uintptr_t(1));
    if (g::createTopologyActivity(activity, &activityOut) != FE_INVALID_ARGS || activityOut ||
        g::createTopologyActivity(activity, nullptr) != FE_INVALID_ARGS)
        return 7;
    std::puts("PASS topology edit/activity ABI defaults, version, size, reserved fields and null outputs");
    return 0;
}

// SPDX-License-Identifier: Apache-2.0
// Public header and exported factory only: no device, Donut, compiler or backend SDK.
#include "GVDBNvrhiApi.h"
#include <cstdio>
namespace g = gvdb::portable;
int main() {
    using namespace nvrhi;
    if (g::gvdbGetNvrhiApiVersion() != g::c_GVDBNvrhiApiVersion) return 1;
    g::VolumeCreateDesc desc;
    auto sentinel = reinterpret_cast<g::IGVDBVolume*>(uintptr_t(1));
    auto output = sentinel;
    if (g::gvdbCreateNvrhiVolume(g::c_GVDBNvrhiApiVersion + 1,&desc,&output) != FE_UNSUPPORTED || output) return 2;
    output = sentinel; desc.structSize--;
    if (g::createVolume(desc,&output) != FE_INVALID_ARGS || output) return 3;
    desc = {}; desc.reserved = 1; output = sentinel;
    if (g::createVolume(desc,&output) != FE_INVALID_ARGS || output) return 4;
    desc = {}; output = sentinel;
    if (g::createVolume(desc,&output) != FE_INVALID_ARGS || output) return 5;
    output = sentinel;
    if (g::gvdbCreateNvrhiVolume(g::c_GVDBNvrhiApiVersion,nullptr,&output) != FE_INVALID_ARGS || output) return 6;
    if (g::createVolume(desc,nullptr) != FE_INVALID_ARGS) return 7;
    std::puts("GVDB public ABI: header layout, factory version/size/reserved/null validation PASS");
    return 0;
}

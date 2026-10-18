// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiReductionApi.h"
#include <cstdio>
int main() {
    namespace g = gvdb::portable;
    using namespace nvrhi;
    g::ReductionCreateDesc desc;
    auto* sentinel=reinterpret_cast<g::IGVDBReduction*>(uintptr_t(1)); auto* output=sentinel;
    if(g::gvdbCreateNvrhiReduction(g::c_GVDBNvrhiApiVersion+1,&desc,&output)!=FE_UNSUPPORTED || output)return 1;
    desc.structSize--;output=sentinel;
    if(g::createReduction(desc,&output)!=FE_INVALID_ARGS || output)return 2;
    desc={};desc.reserved=1;output=sentinel;
    if(g::createReduction(desc,&output)!=FE_INVALID_ARGS || output)return 3;
    desc={};output=sentinel;
    if(g::createReduction(desc,&output)!=FE_INVALID_ARGS || output)return 4;
    output=sentinel;
    if(g::gvdbCreateNvrhiReduction(g::c_GVDBNvrhiApiVersion,nullptr,&output)!=FE_INVALID_ARGS || output)return 5;
    if(g::createReduction(desc,nullptr)!=FE_INVALID_ARGS)return 6;
    std::puts("GVDB public reduction ABI: version/size/reserved/null output rules PASS");return 0;
}

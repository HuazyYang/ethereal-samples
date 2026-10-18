// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiApronApi.h"
#include <cstdio>
int main() {
    namespace g=gvdb::portable;using namespace nvrhi;
    g::ApronCreateDesc desc;g::ApronRequestDesc request;g::ApronBoundaryOverride boundary;
    if(request.structSize!=32||request.overrideCount||request.overrides||request.channelMask||request.reserved2)return 1;
    if(boundary.channel||boundary.reserved[0]||boundary.reserved[1]||boundary.reserved[2])return 2;
    auto* sentinel=reinterpret_cast<g::IGVDBApronOps*>(uintptr_t(1));auto* output=sentinel;
    if(g::gvdbCreateNvrhiApronOps(g::c_GVDBNvrhiApiVersion+1,&desc,&output)!=FE_UNSUPPORTED||output)return 3;
    desc.structSize--;output=sentinel;if(g::createApronOps(desc,&output)!=FE_INVALID_ARGS||output)return 4;
    desc={};desc.reserved=1;output=sentinel;if(g::createApronOps(desc,&output)!=FE_INVALID_ARGS||output)return 5;
    desc={};output=sentinel;if(g::createApronOps(desc,&output)!=FE_INVALID_ARGS||output)return 6;
    output=sentinel;if(g::gvdbCreateNvrhiApronOps(g::c_GVDBNvrhiApiVersion,nullptr,&output)!=FE_INVALID_ARGS||output)return 7;
    if(g::createApronOps(desc,nullptr)!=FE_INVALID_ARGS)return 8;
    std::puts("GVDB public apron ABI: size/defaults/version/reserved/null output rules PASS");return 0;
}

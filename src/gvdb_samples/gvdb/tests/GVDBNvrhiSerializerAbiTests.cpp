// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiSerializer.h"
#include <cstdio>
int main() {
    namespace g = gvdb::portable;
    using namespace nvrhi;
    g::SerializerCreateDesc desc;
    auto* sentinel=reinterpret_cast<g::IGVDBSerializer*>(uintptr_t(1)); auto* output=sentinel;
    if(g::gvdbCreateNvrhiSerializer(g::c_GVDBNvrhiApiVersion+1,&desc,&output)!=FE_UNSUPPORTED || output)return 1;
    desc.structSize--;output=sentinel;
    if(g::createSerializer(desc,&output)!=FE_INVALID_ARGS || output)return 2;
    desc={};desc.reserved=1;output=sentinel;
    if(g::createSerializer(desc,&output)!=FE_INVALID_ARGS || output)return 3;
    desc={};output=sentinel;
    if(g::createSerializer(desc,&output)!=FE_INVALID_ARGS || output)return 4;
    output=sentinel;
    if(g::gvdbCreateNvrhiSerializer(g::c_GVDBNvrhiApiVersion,nullptr,&output)!=FE_INVALID_ARGS || output)return 5;
    if(g::createSerializer(desc,nullptr)!=FE_INVALID_ARGS)return 6;
    std::puts("GVDB public serializer ABI: version/size/reserved/null output rules PASS");return 0;
}

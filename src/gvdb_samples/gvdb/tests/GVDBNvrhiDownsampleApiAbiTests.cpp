// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiDownsampleApi.h"
#include <cstdio>
int main(){
    namespace g=gvdb::portable;using namespace nvrhi;
    g::DownsampleCreateDesc desc;g::DownsampleRequestDesc request;g::DownsamplePreparationBudget budget;
    if(request.structSize!=144||request.reserved[0]||request.reserved[1]||request.reserved[2]||request.reserved2||request.reserved3||request.sourceData||request.sourceBytes)return 1;
    if(budget.maxInputBytes!=UINT64_MAX||budget.maxSampleVisits!=UINT64_MAX||budget.maxBuffers!=UINT32_MAX||budget.reserved)return 2;
    auto* sentinel=reinterpret_cast<g::IGVDBDownsample*>(uintptr_t(1));auto* output=sentinel;
    if(g::gvdbCreateNvrhiDownsample(g::c_GVDBNvrhiApiVersion+1,&desc,&output)!=FE_UNSUPPORTED||output)return 3;
    desc.structSize--;output=sentinel;if(g::createDownsample(desc,&output)!=FE_INVALID_ARGS||output)return 4;
    desc={};desc.reserved=1;output=sentinel;if(g::createDownsample(desc,&output)!=FE_INVALID_ARGS||output)return 5;
    desc={};output=sentinel;if(g::createDownsample(desc,&output)!=FE_INVALID_ARGS||output)return 6;
    output=sentinel;if(g::gvdbCreateNvrhiDownsample(g::c_GVDBNvrhiApiVersion,nullptr,&output)!=FE_INVALID_ARGS||output)return 7;
    if(g::createDownsample(desc,nullptr)!=FE_INVALID_ARGS)return 8;
    std::puts("GVDB downsample ABI defaults/version/size/reserved/null outputs PASS");return 0;
}

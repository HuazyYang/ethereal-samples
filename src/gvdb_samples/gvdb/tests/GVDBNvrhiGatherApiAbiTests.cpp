// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiGatherApi.h"
#include <cstdio>
int main() {
    namespace g=gvdb::portable;
    using namespace nvrhi;
    static_assert(uint32_t(g::GatherKind::Density)==0 && uint32_t(g::GatherKind::LevelSet)==1);
    g::GatherEvaluationDesc evaluation;
    static_assert(std::is_base_of_v<g::IGVDBGather,g::IGVDBGatherEvaluation>);
    if(evaluation.structSize!=80 || evaluation.reserved || evaluation.evaluationPoints || evaluation.request.structSize!=64)return 9;
    g::GatherRequestDesc request;
    g::GatherPreparationBudget budget;
    if(request.structSize!=64 || request.colorChannel!=UINT32_MAX || request.subcellWidth!=2 || request.radius!=1 || request.accumulate || request.supportRadius!=-1 || request.reserved2)return 1;
    if(budget.maxCells!=UINT64_MAX || budget.maxCandidateVisits!=UINT64_MAX || budget.maxPointPairs!=UINT64_MAX || budget.maxAllocatedBytes!=UINT64_MAX || budget.maxBuffers!=UINT32_MAX)return 2;
    g::GatherCreateDesc desc;
    auto* sentinel=reinterpret_cast<g::IGVDBGather*>(uintptr_t(1));auto* output=sentinel;
    if(g::gvdbCreateNvrhiGather(g::c_GVDBNvrhiApiVersion+1,&desc,&output)!=FE_UNSUPPORTED || output)return 3;
    desc.structSize--;output=sentinel;
    if(g::createGather(desc,&output)!=FE_INVALID_ARGS || output)return 4;
    desc={};desc.reserved=1;output=sentinel;
    if(g::createGather(desc,&output)!=FE_INVALID_ARGS || output)return 5;
    desc={};output=sentinel;
    if(g::createGather(desc,&output)!=FE_INVALID_ARGS || output)return 6;
    output=sentinel;
    if(g::gvdbCreateNvrhiGather(g::c_GVDBNvrhiApiVersion,nullptr,&output)!=FE_INVALID_ARGS || output)return 7;
    if(g::createGather(desc,nullptr)!=FE_INVALID_ARGS)return 8;
    std::puts("GVDB public gather ABI: defaults/version/size/reserved/null output rules PASS");return 0;
}

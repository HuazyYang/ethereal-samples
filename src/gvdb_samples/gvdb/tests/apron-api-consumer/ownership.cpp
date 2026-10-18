#include "GVDBNvrhiVoxelOps.h"
int main(){
    using namespace gvdb::portable;
    // Link both overloads without making a call on an invalid object.
    using WithOverrides=nvrhi::FRESULT(NvrhiVoxelOps::*)(uint32_t,const Float4*,uint32_t,const PreparationBudget&,std::unique_ptr<PreparedGeneration>&) noexcept;
    using Defaults=nvrhi::FRESULT(NvrhiVoxelOps::*)(uint32_t,const PreparationBudget&,std::unique_ptr<PreparedGeneration>&) noexcept;
    volatile WithOverrides extended=static_cast<WithOverrides>(&NvrhiVoxelOps::prepareApron);
    volatile Defaults original=static_cast<Defaults>(&NvrhiVoxelOps::prepareApron);
    std::unique_ptr<NvrhiVoxelOps> op;
    return extended&&original&&NvrhiVoxelOps::create({},op)==nvrhi::FE_INVALID_ARGS&&!op?0:1;
}

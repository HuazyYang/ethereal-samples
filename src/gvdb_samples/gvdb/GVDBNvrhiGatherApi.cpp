// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiGatherApi.h"
#include "GVDBNvrhiApiInternal.h"
#include "GVDBNvrhiPointApiInternal.h"
#include "GVDBNvrhiGather.h"
#include <nvrhi/core/foundation.h>
#include <cmath>
#include <new>
#include <stdexcept>
namespace gvdb::portable {
namespace {
using namespace nvrhi;
struct Failure { FRESULT result; };
void require(bool condition,FRESULT result=FE_INVALID_ARGS) {if(!condition)throw Failure{result};}
void check(FRESULT result) {require(NVRHI_SUCCEEDED(result),result);}
template<class F> FRESULT guarded(F&& function) noexcept {
    try{return function();}
    catch(const Failure& error){return error.result;}
    catch(const std::bad_alloc&){return FE_OUT_OF_MEMORY;}
    catch(const std::length_error&){return FE_OUT_OF_MEMORY;}
    catch(...){return FE_GENERIC_ERROR;}
}
class Gather final:public ObjectImpl<IGVDBGather> {
    VolumeHandle m_volume; // Released last: private operator borrows this stable owner.
    std::unique_ptr<NvrhiGather> m_gather;
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Gather)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBGather)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    Gather(VolumeHandle volume,std::unique_ptr<NvrhiGather> gather) noexcept
        :m_volume(std::move(volume)),m_gather(std::move(gather)){}
    IGVDBVolume* getVolume() const noexcept override{return m_volume.Get();}
    FRESULT prepare(const GatherRequestDesc& desc,const GatherPreparationBudget& budget,
        IGVDBGenerationTicket** output) noexcept override {
        if(!output)return FE_INVALID_ARGS;*output=nullptr;
        return guarded([&] {
            require(desc.structSize==sizeof(GatherRequestDesc)&&!desc.reserved&&!desc.reserved2&&!budget.reserved);
            require(desc.accumulate<=1);
            require(desc.supportRadius==-1 || (std::isfinite(desc.supportRadius)&&desc.supportRadius>=0));
            require(std::isfinite(desc.translation.x)&&std::isfinite(desc.translation.y)&&std::isfinite(desc.translation.z)&&std::isfinite(desc.translation.w));
            detail::ResolvedPointSet points;
            check(detail::resolvePointSet(desc.points,m_volume->getDevice(),points));
            std::unique_ptr<PreparedGeneration> ticket;
            check(m_gather->prepare(desc,budget,points,ticket));
            return detail::wrapGeneration(m_volume->getDevice(),std::move(ticket),output);
        });
    }
};
} // namespace
nvrhi::FRESULT gvdbCreateNvrhiGather(uint32_t apiVersion,const GatherCreateDesc* desc,IGVDBGather** output) noexcept {
    if(!output)return FE_INVALID_ARGS;*output=nullptr;
    return guarded([&] {
        require(apiVersion==c_GVDBNvrhiApiVersion,FE_UNSUPPORTED);
        require(desc&&desc->structSize==sizeof(GatherCreateDesc)&&!desc->reserved&&desc->volume&&desc->artifact&&desc->artifactBytes&&desc->artifactBytes<=SIZE_MAX);
        VolumeHandle volume=desc->volume;NvrhiVolume* owner=nullptr;check(detail::resolveVolume(volume,&owner));
        std::unique_ptr<NvrhiGather> gather;check(NvrhiGather::create({owner,desc->artifact,size_t(desc->artifactBytes)},gather));
        auto result=MAKE_RC_OBJ_PTR(Gather,std::move(volume),std::move(gather));*output=result.Detach();return FS_OK;
    });
}
} // namespace gvdb::portable

// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiResampleApi.h"
#include "GVDBNvrhiApiInternal.h"
#include "GVDBNvrhiResample.h"
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
class Resample final:public ObjectImpl<IGVDBResample> {
    VolumeHandle m_volume; // Released last: concrete operator borrows its stable owner.
    std::unique_ptr<NvrhiResample> m_resample;
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Resample)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBResample)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    Resample(VolumeHandle volume,std::unique_ptr<NvrhiResample> resample) noexcept
        :m_volume(std::move(volume)),m_resample(std::move(resample)){}
    IGVDBVolume* getVolume() const noexcept override{return m_volume.Get();}
    FRESULT prepare(const ResampleRequestDesc& desc,const ResamplePreparationBudget& budget,
        IGVDBGenerationTicket** output) noexcept override {
        if(!output)return FE_INVALID_ARGS;*output=nullptr;
        return guarded([&] {
            require(desc.structSize==sizeof(ResampleRequestDesc)&&!desc.reserved&&!desc.reserved2&&!budget.reserved);
            require(desc.preserveOutside<=1);
            require(std::isfinite(desc.background.x)&&std::isfinite(desc.background.y)&&std::isfinite(desc.background.z)&&std::isfinite(desc.background.w));
            ResampleRequest request;
            request.channel=desc.channel;request.source={desc.sourceData,desc.sourceBytes,desc.sourceResolution};
            for(uint32_t i=0;i<3;++i)request.transformRows[i]=desc.transformRows[i];
            request.remap=desc.remap;request.preserveOutside=desc.preserveOutside!=0;request.background=desc.background.x;
            std::unique_ptr<PreparedGeneration> ticket;
            check(m_resample->prepare(request,{budget.maxAllocatedBytes,budget.maxBuffers},ticket));
            return detail::wrapGeneration(m_volume->getDevice(),std::move(ticket),output);
        });
    }
};
} // namespace
nvrhi::FRESULT gvdbCreateNvrhiResample(uint32_t apiVersion,const ResampleCreateDesc* desc,IGVDBResample** output) noexcept {
    if(!output)return FE_INVALID_ARGS;*output=nullptr;
    return guarded([&] {
        require(apiVersion==c_GVDBNvrhiApiVersion,FE_UNSUPPORTED);
        require(desc&&desc->structSize==sizeof(ResampleCreateDesc)&&!desc->reserved&&desc->volume&&desc->artifact&&desc->artifactBytes&&desc->artifactBytes<=SIZE_MAX);
        VolumeHandle volume=desc->volume;NvrhiVolume* owner=nullptr;check(detail::resolveVolume(volume,&owner));
        std::unique_ptr<NvrhiResample> resample;check(NvrhiResample::create({owner,desc->artifact,size_t(desc->artifactBytes)},resample));
        auto result=MAKE_RC_OBJ_PTR(Resample,std::move(volume),std::move(resample));*output=result.Detach();return FS_OK;
    });
}
} // namespace gvdb::portable

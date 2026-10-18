// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiReductionApi.h"
#include "GVDBNvrhiApiInternal.h"
#include "GVDBNvrhiReduction.h"
#include <nvrhi/core/foundation.h>
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
NVRHI_CLASS_CLSID(ReductionTicket,"1c3ad321-c5e6-4c8d-8e89-7beff1f55264")
class ReductionTicket final:public ObjectImpl<IGVDBReductionTicket> {
    DeviceHandle m_device; // Released after every GPU resource in m_ticket.
 public:
    std::unique_ptr<PreparedReduction> m_ticket;
    NVRHI_DECLARE_UUID_TRAITS(ReductionTicket)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ReductionTicket)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBReductionTicket)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(ReductionTicket)
    NVRHI_END_INTERFACE_TABLE()
    ReductionTicket(IDevice* device,std::unique_ptr<PreparedReduction> ticket) noexcept
        :m_device(device),m_ticket(std::move(ticket)){}
    IDevice* getDevice() const noexcept override{return m_device.Get();}
    FRESULT finishAfterCallerCompletion(ReductionValue& output) noexcept override {
        ReductionResult result;
        const auto status=NvrhiReduction::finishAfterCallerCompletion(*m_ticket,result);
        if(NVRHI_SUCCEEDED(status))output={result.sum,0,result.voxelCount};
        return status;
    }
};
class Reduction final:public ObjectImpl<IGVDBReduction> {
    VolumeHandle m_volume; // Released last: core preparation borrows its stable owner.
    std::unique_ptr<NvrhiReduction> m_reduction;
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Reduction)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBReduction)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    Reduction(VolumeHandle volume,std::unique_ptr<NvrhiReduction> reduction) noexcept
        :m_volume(std::move(volume)),m_reduction(std::move(reduction)){}
    IGVDBVolume* getVolume() const noexcept override{return m_volume.Get();}
    FRESULT prepare(IGVDBSnapshot* snapshot,uint32_t channel,const ReductionPreparationBudget& budget,
        IGVDBReductionTicket** output) noexcept override {
        if(!output)return FE_INVALID_ARGS;*output=nullptr;
        return guarded([&] {
            require(!budget.reserved);
            std::shared_ptr<const VolumeGeneration> generation;check(detail::resolveSnapshot(snapshot,generation));
            std::unique_ptr<PreparedReduction> ticket;
            check(m_reduction->prepare(std::move(generation),channel,{budget.maxAllocatedBytes,budget.maxBuffers},ticket));
            auto result=MAKE_RC_OBJ_PTR(ReductionTicket,m_volume->getDevice(),std::move(ticket));
            *output=result.Detach();return FS_OK;
        });
    }
    FRESULT record(RecordingContext context,IGVDBReductionTicket* ticket) noexcept override {
        return guarded([&] {
            require(ticket!=nullptr);AutoPtr<ReductionTicket> value;
            require(NVRHI_SUCCEEDED(ticket->QueryInterface(uuid_of<ReductionTicket>(),reinterpret_cast<void**>(value.GetAddressOf())))&&value.Get());
            return m_reduction->record(context,*value->m_ticket);
        });
    }
};
} // namespace
nvrhi::FRESULT gvdbCreateNvrhiReduction(uint32_t apiVersion,const ReductionCreateDesc* desc,IGVDBReduction** output) noexcept {
    if(!output)return FE_INVALID_ARGS;*output=nullptr;
    return guarded([&] {
        require(apiVersion==c_GVDBNvrhiApiVersion,FE_UNSUPPORTED);
        require(desc&&desc->structSize==sizeof(ReductionCreateDesc)&&!desc->reserved&&desc->volume&&desc->artifact&&desc->artifactBytes&&desc->artifactBytes<=SIZE_MAX);
        VolumeHandle volume=desc->volume;NvrhiVolume* owner=nullptr;check(detail::resolveVolume(volume,&owner));
        std::unique_ptr<NvrhiReduction> reduction;check(NvrhiReduction::create({owner,desc->artifact,size_t(desc->artifactBytes)},reduction));
        auto result=MAKE_RC_OBJ_PTR(Reduction,std::move(volume),std::move(reduction));*output=result.Detach();return FS_OK;
    });
}
} // namespace gvdb::portable

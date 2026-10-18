// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiDownsampleApi.h"
#include "GVDBNvrhiDownsample.h"
#include <nvrhi/core/foundation.h>
#include <new>
#include <stdexcept>
namespace gvdb::portable {
namespace {
using namespace nvrhi;
struct Failure{FRESULT result;};
void require(bool value,FRESULT result=FE_INVALID_ARGS){if(!value)throw Failure{result};}
void check(FRESULT result){require(NVRHI_SUCCEEDED(result),result);}
template<class F>FRESULT guarded(F&& fn)noexcept{
    try{return fn();}catch(const Failure& e){return e.result;}
    catch(const std::bad_alloc&){return FE_OUT_OF_MEMORY;}
    catch(const std::length_error&){return FE_OUT_OF_MEMORY;}
    catch(...){return FE_GENERIC_ERROR;}
}
NVRHI_CLASS_CLSID(DownsampleTicket,"b328e912-a122-4655-9b83-43bf7f1c1d3f")
class DownsampleTicket final:public ObjectImpl<IGVDBDownsampleTicket>{
 public:
    std::unique_ptr<PreparedDownsample> ticket;
    NVRHI_DECLARE_UUID_TRAITS(DownsampleTicket)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(DownsampleTicket)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBDownsampleTicket)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(DownsampleTicket)
    NVRHI_END_INTERFACE_TABLE()
    explicit DownsampleTicket(std::unique_ptr<PreparedDownsample> value)noexcept:ticket(std::move(value)){}
    IDevice* getDevice()const noexcept override{return ticket->getDevice();}
    UInt3 getDestinationResolution()const noexcept override{return ticket->getDestinationResolution();}
    FRESULT finishAfterCallerCompletion(BorrowedBytes& output)noexcept override{return NvrhiDownsample::finishAfterCallerCompletion(*ticket,output);}
    FRESULT getValues(BorrowedBytes& output)const noexcept override{return ticket->getValues(output);}
};
class Downsample final:public ObjectImpl<IGVDBDownsample>{
    std::unique_ptr<NvrhiDownsample> m_downsample;
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Downsample)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBDownsample)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    explicit Downsample(std::unique_ptr<NvrhiDownsample> value)noexcept:m_downsample(std::move(value)){}
    IDevice* getDevice()const noexcept override{return m_downsample->getDevice();}
    FRESULT prepare(const DownsampleRequestDesc& desc,const DownsamplePreparationBudget& budget,IGVDBDownsampleTicket** output)noexcept override{
        if(!output)return FE_INVALID_ARGS;*output=nullptr;
        return guarded([&]{std::unique_ptr<PreparedDownsample> ticket;check(m_downsample->prepare(desc,budget,ticket));
            auto result=MAKE_RC_OBJ_PTR(DownsampleTicket,std::move(ticket));*output=result.Detach();return FS_OK;});
    }
    FRESULT record(RecordingContext context,IGVDBDownsampleTicket* ticket)noexcept override{
        return guarded([&]{require(ticket!=nullptr);AutoPtr<DownsampleTicket> value;
            require(NVRHI_SUCCEEDED(ticket->QueryInterface(uuid_of<DownsampleTicket>(),reinterpret_cast<void**>(value.GetAddressOf())))&&value.Get());
            return m_downsample->record(context,*value->ticket);});
    }
};
} // namespace
nvrhi::FRESULT gvdbCreateNvrhiDownsample(uint32_t version,const DownsampleCreateDesc* desc,IGVDBDownsample** output)noexcept{
    if(!output)return FE_INVALID_ARGS;*output=nullptr;
    return guarded([&]{require(version==c_GVDBNvrhiApiVersion,FE_UNSUPPORTED);require(desc!=nullptr);
        std::unique_ptr<NvrhiDownsample> value;check(NvrhiDownsample::create(*desc,value));
        auto result=MAKE_RC_OBJ_PTR(Downsample,std::move(value));*output=result.Detach();return FS_OK;});
}
} // namespace gvdb::portable

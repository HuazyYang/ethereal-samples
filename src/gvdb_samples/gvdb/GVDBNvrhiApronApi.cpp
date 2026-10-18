// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiApronApi.h"
#include "GVDBNvrhiApiInternal.h"
#include "GVDBNvrhiVoxelOps.h"
#include <nvrhi/core/foundation.h>
#include <array>
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
class ApronOps final:public ObjectImpl<IGVDBApronOps> {
    VolumeHandle m_volume;
    std::unique_ptr<NvrhiVoxelOps> m_ops;
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ApronOps)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBApronOps)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    ApronOps(VolumeHandle volume,std::unique_ptr<NvrhiVoxelOps> ops) noexcept
        :m_volume(std::move(volume)),m_ops(std::move(ops)){}
    IGVDBVolume* getVolume() const noexcept override{return m_volume.Get();}
    FRESULT prepare(const ApronRequestDesc& desc,const ApronPreparationBudget& budget,
        IGVDBGenerationTicket** output) noexcept override {
        if(!output)return FE_INVALID_ARGS;*output=nullptr;
        return guarded([&] {
            require(desc.structSize==sizeof(ApronRequestDesc)&&!desc.reserved&&!desc.reserved2&&!budget.reserved);
            require(desc.overrideCount<=c_MaxChannels&&(!desc.overrideCount||desc.overrides));
            std::array<Float4,c_MaxChannels> values{};uint32_t mask=0;
            for(uint32_t i=0;i<desc.overrideCount;++i) {
                const auto& item=desc.overrides[i];
                require(item.channel<c_MaxChannels&&!item.reserved[0]&&!item.reserved[1]&&!item.reserved[2]);
                const uint32_t bit=uint32_t(1)<<item.channel;
                require((desc.channelMask&bit)&&!(mask&bit));mask|=bit;values[item.channel]=item.value;
            }
            std::unique_ptr<PreparedGeneration> ticket;
            check(m_ops->prepareApron(desc.channelMask,values.data(),mask,{budget.maxAllocatedBytes,budget.maxBuffers},ticket));
            return detail::wrapGeneration(m_volume->getDevice(),std::move(ticket),output);
        });
    }
};
} // namespace
nvrhi::FRESULT gvdbCreateNvrhiApronOps(uint32_t apiVersion,const ApronCreateDesc* desc,IGVDBApronOps** output) noexcept {
    if(!output)return FE_INVALID_ARGS;*output=nullptr;
    return guarded([&] {
        require(apiVersion==c_GVDBNvrhiApiVersion,FE_UNSUPPORTED);
        require(desc&&desc->structSize==sizeof(ApronCreateDesc)&&!desc->reserved&&desc->volume&&desc->artifact&&desc->artifactBytes&&desc->artifactBytes<=SIZE_MAX);
        VolumeHandle volume=desc->volume;NvrhiVolume* owner=nullptr;check(detail::resolveVolume(volume,&owner));
        std::unique_ptr<NvrhiVoxelOps> ops;check(NvrhiVoxelOps::create({owner,desc->artifact,size_t(desc->artifactBytes)},ops));
        auto result=MAKE_RC_OBJ_PTR(ApronOps,std::move(volume),std::move(ops));*output=result.Detach();return FS_OK;
    });
}
} // namespace gvdb::portable

// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiVoxelApi.h"
#include "GVDBNvrhiApiInternal.h"
#include "GVDBNvrhiVoxelOps.h"
#include <nvrhi/core/foundation.h>
#include <new>
#include <stdexcept>
namespace gvdb::portable {
namespace {
using namespace nvrhi;
struct Failure { FRESULT result; };
void require(bool condition, FRESULT result = FE_INVALID_ARGS) { if (!condition) throw Failure{result}; }
void check(FRESULT result) { require(NVRHI_SUCCEEDED(result),result); }
template<class F> FRESULT guarded(F&& function) noexcept {
    try { return function(); }
    catch (const Failure& failure) { return failure.result; }
    catch (const std::bad_alloc&) { return FE_OUT_OF_MEMORY; }
    catch (const std::length_error&) { return FE_OUT_OF_MEMORY; }
    catch (...) { return FE_GENERIC_ERROR; }
}
class VoxelOps final : public ObjectImpl<IGVDBVoxelOps> {
    VolumeHandle m_volume; // Destroyed last: the core operator borrows its stable owner.
    std::unique_ptr<NvrhiVoxelOps> m_ops;
    template<class F> FRESULT prepare(const VoxelPreparationBudget& budget,
        IGVDBGenerationTicket** output, F&& operation) noexcept {
        if (!output) return FE_INVALID_ARGS;
        *output = nullptr;
        return guarded([&] {
            require(!budget.reserved);
            std::unique_ptr<PreparedGeneration> ticket;
            check(operation(PreparationBudget{budget.maxAllocatedBytes,budget.maxBuffers},ticket));
            return detail::wrapGeneration(m_volume->getDevice(),std::move(ticket),output);
        });
    }
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(VoxelOps)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBVoxelOps)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    VoxelOps(VolumeHandle volume, std::unique_ptr<NvrhiVoxelOps> ops) noexcept
        : m_volume(std::move(volume)),m_ops(std::move(ops)) {}
    IGVDBVolume* getVolume() const noexcept override { return m_volume.Get(); }
    FRESULT prepareFill(uint32_t channel, Float4 value, const VoxelPreparationBudget& budget,
        IGVDBGenerationTicket** output) noexcept override {
        return prepare(budget,output,[&](PreparationBudget limits, auto& ticket) {
            return m_ops->prepareFill(channel,value,limits,ticket);
        });
    }
    FRESULT prepareClear(uint32_t mask, const VoxelPreparationBudget& budget,
        IGVDBGenerationTicket** output) noexcept override {
        return prepare(budget,output,[&](PreparationBudget limits, auto& ticket) {
            return m_ops->prepareClear(mask,limits,ticket);
        });
    }
    FRESULT prepareCopy(uint32_t source, uint32_t destination, const VoxelPreparationBudget& budget,
        IGVDBGenerationTicket** output) noexcept override {
        return prepare(budget,output,[&](PreparationBudget limits, auto& ticket) {
            return m_ops->prepareCopy(source,destination,limits,ticket);
        });
    }
    FRESULT prepareApron(uint32_t mask, const VoxelPreparationBudget& budget,
        IGVDBGenerationTicket** output) noexcept override {
        return prepare(budget,output,[&](PreparationBudget limits, auto& ticket) {
            return m_ops->prepareApron(mask,limits,ticket);
        });
    }
};
}
nvrhi::FRESULT gvdbCreateNvrhiVoxelOps(uint32_t apiVersion, const VoxelOpsCreateDesc* desc,
                                     IGVDBVoxelOps** output) noexcept {
    if (!output) return nvrhi::FE_INVALID_ARGS;
    *output = nullptr;
    return guarded([&] {
        require(apiVersion == c_GVDBNvrhiApiVersion,nvrhi::FE_UNSUPPORTED);
        require(desc && desc->structSize == sizeof(VoxelOpsCreateDesc));
        require(!desc->reserved && desc->volume && desc->artifact && desc->artifactBytes && desc->artifactBytes <= SIZE_MAX);
        VolumeHandle volume = desc->volume;
        NvrhiVolume* owner = nullptr;
        check(detail::resolveVolume(volume,&owner));
        VoxelOpsDesc config{owner,desc->artifact,size_t(desc->artifactBytes)};
        std::unique_ptr<NvrhiVoxelOps> ops;
        check(NvrhiVoxelOps::create(config,ops));
        auto result = MAKE_RC_OBJ_PTR(VoxelOps,std::move(volume),std::move(ops));
        *output = result.Detach(); return nvrhi::FS_OK;
    });
}
} // namespace gvdb::portable

// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiVoxelFilterApi.h"
#include "GVDBNvrhiApiInternal.h"
#include "GVDBNvrhiVoxelFilters.h"
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
VoxelFilterOp operation(VoxelFilterOperation value) {
    switch (value) {
    case VoxelFilterOperation::Smooth: return VoxelFilterOp::Smooth;
    case VoxelFilterOperation::Grow: return VoxelFilterOp::Grow;
    case VoxelFilterOperation::Noise: return VoxelFilterOp::Noise;
    case VoxelFilterOperation::Cut: return VoxelFilterOp::Cut;
    case VoxelFilterOperation::ExpandByte: return VoxelFilterOp::ExpandByte;
    case VoxelFilterOperation::ExpandColor: return VoxelFilterOp::ExpandColor;
    default: throw Failure{FE_INVALID_ARGS};
    }
}
class VoxelFilters final : public ObjectImpl<IGVDBVoxelFilters> {
    VolumeHandle m_volume; // Destroyed last: the operator borrows the stable core owner.
    std::unique_ptr<NvrhiVoxelFilters> m_filters;
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(VoxelFilters)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBVoxelFilters)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    VoxelFilters(VolumeHandle volume, std::unique_ptr<NvrhiVoxelFilters> filters) noexcept
        : m_volume(std::move(volume)),m_filters(std::move(filters)) {}
    IGVDBVolume* getVolume() const noexcept override { return m_volume.Get(); }
    FRESULT prepare(const VoxelFilterRequest& desc, const VoxelFilterPreparationBudget& budget,
        IGVDBGenerationTicket** output) noexcept override {
        if (!output) return FE_INVALID_ARGS;
        *output = nullptr;
        return guarded([&] {
            require(desc.structSize == sizeof(VoxelFilterRequest) && !desc.reserved && !desc.reserved2 && !budget.reserved);
            require(desc.updateApron <= 1 && desc.skipOverAprons <= 1);
            VoxelFilterDesc request;
            request.operation = operation(desc.operation); request.channel = desc.channel;
            request.iterations = desc.iterations; request.parameters = desc.parameters; request.boundaryValue = desc.boundaryValue;
            request.updateApron = desc.updateApron != 0; request.skipOverAprons = desc.skipOverAprons != 0;
            std::unique_ptr<PreparedGeneration> ticket;
            check(m_filters->prepare(request,{budget.maxAllocatedBytes,budget.maxBuffers},ticket));
            return detail::wrapGeneration(m_volume->getDevice(),std::move(ticket),output);
        });
    }
};
}
nvrhi::FRESULT gvdbCreateNvrhiVoxelFilters(uint32_t apiVersion, const VoxelFiltersCreateDesc* desc,
                                         IGVDBVoxelFilters** output) noexcept {
    if (!output) return nvrhi::FE_INVALID_ARGS;
    *output = nullptr;
    return guarded([&] {
        require(apiVersion == c_GVDBNvrhiApiVersion,FE_UNSUPPORTED);
        require(desc && desc->structSize == sizeof(VoxelFiltersCreateDesc));
        require(!desc->reserved && desc->volume && desc->artifact && desc->artifactBytes && desc->artifactBytes <= SIZE_MAX);
        VolumeHandle volume = desc->volume;
        NvrhiVolume* owner = nullptr;
        check(detail::resolveVolume(volume,&owner));
        std::unique_ptr<NvrhiVoxelFilters> filters;
        check(NvrhiVoxelFilters::create({owner,desc->artifact,size_t(desc->artifactBytes)},filters));
        auto result = MAKE_RC_OBJ_PTR(VoxelFilters,std::move(volume),std::move(filters));
        *output = result.Detach(); return FS_OK;
    });
}
} // namespace gvdb::portable

// SPDX-License-Identifier: Apache-2.0
#pragma once
// Private operator implementation, compiled into the public API binary.
#include "GVDBNvrhiGatherApi.h"
#include "GVDBNvrhiPointApiInternal.h"
#include "GVDBNvrhiVolume.h"
namespace gvdb::portable {
struct GatherOperatorDesc {
    NvrhiVolume* volume = nullptr; // Borrowed; facade retains the owner.
    const void* artifact = nullptr;
    size_t artifactBytes = 0;
    bool inverseDistance = false;
};
class NvrhiGather {
 public:
    ~NvrhiGather();
    NvrhiGather(const NvrhiGather&) = delete;
    NvrhiGather& operator=(const NvrhiGather&) = delete;
    static nvrhi::FRESULT create(const GatherOperatorDesc&,std::unique_ptr<NvrhiGather>& output) noexcept;
    nvrhi::FRESULT prepare(const GatherRequestDesc&,const GatherPreparationBudget&,
        const detail::ResolvedPointSet& associations,const detail::ResolvedPointSet& evaluation,
        std::unique_ptr<PreparedGeneration>& output) noexcept;
 private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    explicit NvrhiGather(std::unique_ptr<Impl>) noexcept;
};
} // namespace gvdb::portable

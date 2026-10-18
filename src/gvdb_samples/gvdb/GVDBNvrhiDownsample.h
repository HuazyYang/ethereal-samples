// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiDownsampleApi.h"
#include <memory>
namespace gvdb::portable {
class PreparedDownsample {
 public:
    ~PreparedDownsample();
    PreparedDownsample(const PreparedDownsample&) = delete;
    PreparedDownsample& operator=(const PreparedDownsample&) = delete;
    nvrhi::IDevice* getDevice() const noexcept;
    UInt3 getDestinationResolution() const noexcept;
    nvrhi::FRESULT getValues(BorrowedBytes&) const noexcept;
 private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    explicit PreparedDownsample(std::unique_ptr<Impl>) noexcept;
    friend class NvrhiDownsample;
};
class NvrhiDownsample {
 public:
    ~NvrhiDownsample();
    NvrhiDownsample(const NvrhiDownsample&) = delete;
    NvrhiDownsample& operator=(const NvrhiDownsample&) = delete;
    static nvrhi::FRESULT create(const DownsampleCreateDesc&,std::unique_ptr<NvrhiDownsample>&) noexcept;
    nvrhi::IDevice* getDevice() const noexcept;
    // Core unique_ptr outputs are preserved on failure; public raw outputs are cleared.
    nvrhi::FRESULT prepare(const DownsampleRequestDesc&,const DownsamplePreparationBudget&,
        std::unique_ptr<PreparedDownsample>&) noexcept;
    nvrhi::FRESULT record(RecordingContext,PreparedDownsample&) noexcept;
    static nvrhi::FRESULT finishAfterCallerCompletion(PreparedDownsample&,BorrowedBytes&) noexcept;
 private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    explicit NvrhiDownsample(std::unique_ptr<Impl>) noexcept;
};
} // namespace gvdb::portable

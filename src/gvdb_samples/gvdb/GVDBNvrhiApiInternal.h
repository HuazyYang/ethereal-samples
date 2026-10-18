// SPDX-License-Identifier: Apache-2.0
#pragma once
// Private implementation seam. Not installed and not exported from the shared SDK.
#include "GVDBNvrhiApi.h"
#include <memory>
namespace gvdb::portable {
class NvrhiVolume;
class PreparedGeneration;
class VolumeGeneration;
namespace detail {
// Authenticates this binary's concrete facade. The result is borrowed and remains
// stable only while the caller retains the supplied public volume interface.
nvrhi::FRESULT resolveVolume(IGVDBVolume*, NvrhiVolume** output) noexcept;
// Authenticates a public snapshot without exposing implementation ownership in the ABI.
nvrhi::FRESULT resolveSnapshot(IGVDBSnapshot*, std::shared_ptr<const VolumeGeneration>& output) noexcept;
// Consumes a newly prepared core ticket and returns the same public ticket type
// accepted by IGVDBVolume::recordAndPublish. Clears output before any failure.
nvrhi::FRESULT wrapGeneration(nvrhi::IDevice*, std::unique_ptr<PreparedGeneration>,
                             IGVDBGenerationTicket** output) noexcept;
} // namespace detail
} // namespace gvdb::portable

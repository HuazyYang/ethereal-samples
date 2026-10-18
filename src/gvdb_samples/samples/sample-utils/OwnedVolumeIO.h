// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gvdb/GVDBNvrhiApi.h>
#include <filesystem>
namespace gvdb::sample {
// Synchronous sample convenience, not part of the public owner core. Uses the
// public serializer and caller device; records/submits an upload list and waits.
// Caller must flush/close any other immediate command list on this device first.
// Failed recording closes the local list without submitting it.
// Returns one retained publication; the temporary owner and serializer are gone.
// Input and remap artifact bytes are copied before they leave scope.
portable::SnapshotHandle loadOwnedVBX(nvrhi::IDevice*, const std::filesystem::path& remapArtifact,
    const std::filesystem::path& input);
portable::SnapshotHandle loadOwnedVBX(nvrhi::IDevice*, const std::filesystem::path& remapArtifact,
    const void* input, size_t inputBytes);
} // namespace gvdb::sample

// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gvdb/GVDBNvrhiApi.h>
#include <filesystem>
namespace gvdb::sample
{
struct OwnedVolumeLoadOptions
{
    // Explicit opt-in: regenerate these channel aprons through the public voxel
    // operator after exact import. Zero preserves all imported logical bytes.
    uint32_t refreshApronChannels = 0;
    std::filesystem::path mutationArtifact;
};
// Synchronous sample convenience, not part of the public owner core. Uses the
// public serializer and caller device; records/submits an upload list and waits.
// Caller must flush/close any other immediate command list on this device first.
// Failed recording closes the local list without submitting it.
// Returns one retained publication; the temporary owner and serializer are gone.
// Input and remap artifact bytes are copied before they leave scope.
portable::SnapshotHandle loadOwnedVBX(nvrhi::IDevice *, const std::filesystem::path &remapArtifact,
                                      const std::filesystem::path &input, const OwnedVolumeLoadOptions & = {});
portable::SnapshotHandle loadOwnedVBX(nvrhi::IDevice *, const std::filesystem::path &remapArtifact, const void *input,
                                      size_t inputBytes, const OwnedVolumeLoadOptions & = {});
} // namespace gvdb::sample

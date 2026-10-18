// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gvdb/GVDBNvrhiApi.h>
#include <gvdb/GVDBSlangVolume.h>
#include <filesystem>
#include <memory>
namespace gvdb::sample
{
// App-only renderer of one immutable public publication. No duplicate atlas or
// topology. Construct with shaders/gvdb_owned/<backend>/OwnedVolumeSurface_renderSurface.bin.
class OwnedVolumeSurface
{
  public:
    OwnedVolumeSurface(nvrhi::IDevice *, const std::filesystem::path &artifact);
    ~OwnedVolumeSurface();
    OwnedVolumeSurface(const OwnedVolumeSurface &) = delete;
    OwnedVolumeSurface &operator=(const OwnedVolumeSurface &) = delete;
    // Flush/close other immediate lists first. Records and submits on Graphics;
    // never waits. Later same-queue rendering/readback is ordered normally, and
    // cross-queue synchronization remains the caller's responsibility. Bindings
    // retain resources after the snapshot, owner, or renderer is released.
    // colorChannel=UINT32_MAX uses transfer color; otherwise requires RGBA8_UINT
    // in this same snapshot. Scalar is R32_FLOAT. Missing cells use background,
    // ignoring stored aprons; color is point-filtered and gradients point outward.
    slang::RaycastOutput render(portable::SnapshotHandle snapshot, uint32_t scalarChannel, uint32_t colorChannel,
                                slang::RaycastParameters parameters, const std::vector<slang::Float4> &transfer,
                                float scalarBackground);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace gvdb::sample

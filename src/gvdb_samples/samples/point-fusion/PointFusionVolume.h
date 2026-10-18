// Persistent point fusion through the public owned-volume and point APIs.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PointFusionScanner.h"
#include <gvdb/GVDBNvrhiPointApi.h>
#include <filesystem>
#include <memory>
namespace gvdb::samples
{
struct PointFusionConfiguration
{
    float radius = 1, support = 2;
    bool color = false;
};
class PointFusionVolume
{
    struct State;
    ethereal::slang::ComputeRuntime *m_runtime;
    PointFusionConfiguration m_configuration;
    std::filesystem::path m_artifacts;
    std::unique_ptr<State> m_state;
    uint64_t m_frames = 0, m_hits = 0;
    std::unique_ptr<State> createState(bool color);

  public:
    explicit PointFusionVolume(ethereal::slang::ComputeRuntime &, PointFusionConfiguration = {},
                               std::filesystem::path artifacts = {});
    ~PointFusionVolume();
    // Compact scanner output is explicitly staged to CPU, then converted into an
    // owned PointSet. Publication is atomic per operator, not per multi-step frame.
    // A failed post-topology operation leaves the last rendered snapshot intact;
    // reset is required before another fuse. Device failures cannot be rolled back.
    void fuse(const ScanFrame &);
    void reset(bool color);
    bool hasVolume() const;
    portable::SnapshotHandle snapshot() const;
    const portable::IGVDBHostVolume &topology() const;
    portable::PointSetHandle lastPoints() const;
    // Test/export convenience: explicit synchronous public readback of the current
    // retained snapshot. The core API itself never waits or owns our command list.
    portable::ReadbackHandle readback(uint32_t channelMask) const;
    bool useColor() const
    {
        return m_configuration.color;
    }
    uint64_t frames() const
    {
        return m_frames;
    }
    uint64_t totalHits() const
    {
        return m_hits;
    }
};
} // namespace gvdb::samples

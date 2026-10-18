// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gvdb/GVDBNvrhiApi.h>
#include <gvdb/GVDBSlangVolume.h>
namespace gvdb::sample {
// App-only renderer: ComputeRuntime owns transient image/TF buffers and submission,
// while the public snapshot is the sole source of topology and canonical raw atlas.
// No volume buffers or CPU PackedVolume are duplicated. Pass a newly acquired
// snapshot after publication; all bindings/metadata are taken from this one handle.
// The snapshot must come from the public owner factory on runtime.device().
// Recorded binding sets retain resources through execution, including after the
// caller releases both snapshot and owner. Submission ordering remains the caller's.
// Artifacts live in shaders/gvdb_owned/<backend>, built independently of tests.
slang::RaycastOutput raycastOwnedVolume(ethereal::slang::ComputeRuntime& runtime,
    portable::SnapshotHandle snapshot, uint32_t channel, slang::RaycastParameters parameters,
    const std::vector<slang::Float4>& transfer, bool surface = false);
} // namespace gvdb::sample

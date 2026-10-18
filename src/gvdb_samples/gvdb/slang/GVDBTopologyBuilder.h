// CPU sparse topology construction without legacy CUDA initialization.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBPortableData.h"
namespace gvdb::slang {
struct TopologyBuildOptions {
    std::vector<uint32_t> logDimensions{3,3,3,3};
    uint32_t apron=1;
    UInt4 atlasGrid{}; // xyz zero: choose a compact grid; w ignored
};
struct ScalarVolumeData {PackedVolume packed;std::vector<float> atlas;};
// Leaf origins must be aligned to the level-zero width. Duplicates coalesce.
// Parent coordinates are relative to a root anchored at the minimum leaf origin,
// allowing negative coordinates without wrapping the serialized references.
bool buildTopology(const std::vector<Int4>& leafOrigins,const TopologyBuildOptions& options,
    PackedVolume& output,std::string& error);
// Activates any leaf containing abs(value-background)>threshold. Carries all
// source values in active leaves and fills aprons from active neighbor leaves.
// No-active-leaf input returns a diagnostic; output is unchanged on failure.
bool buildScalarVolume(const float* values,size_t count,UInt4 resolution,const TopologyBuildOptions& options,
    float threshold,float background,ScalarVolumeData& output,std::string& error);
// Same dense-to-sparse construction at a leaf-aligned signed index origin.
// Uniform translation preserves child ordering, references and all atlas bytes.
bool buildScalarVolumeAtOrigin(const float* values,size_t count,UInt4 resolution,Int4 origin,
    const TopologyBuildOptions& options,float threshold,float background,ScalarVolumeData& output,std::string& error);
} // namespace gvdb::slang

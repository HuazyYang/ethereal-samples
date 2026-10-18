// CPU packing of serialized GVDB pools for portable GPU execution.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBPortable.h"
#include <string>
#include <vector>
namespace gvdb::slang {
struct PoolSource { const void* data = nullptr; uint64_t count = 0; uint64_t stride = 0; };
struct LevelSource { PoolSource nodes; PoolSource children; uint32_t logDimension = 0; };
struct VolumeSource {
    VolumeInfo volume{};
    std::vector<LevelSource> levels;
    const AtlasNode* atlasMap = nullptr;
    size_t atlasMapCount = 0;
};
struct PackedVolume {
    VolumeInfo volume{};
    std::vector<LevelInfo> levels;
    std::vector<uint32_t> nodes;
    std::vector<uint32_t> children;
    std::vector<AtlasNode> atlasMap;
};
// Validates before copying. A failed pack leaves output unchanged and describes
// the actual invalid/unsupported field. Does not access or synchronize any GPU.
bool packVolume(const VolumeSource& source, PackedVolume& output, std::string& error);
} // namespace gvdb::slang

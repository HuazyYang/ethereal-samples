// Packed point-frame input, independent of graphics APIs and NVIDIA SDKs.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gvdb/slang/GVDBPortable.h>
#include <filesystem>
#include <istream>
#include <vector>
namespace gvdb::samples::pointcloud {
struct PointFrame {
    slang::Float4 minimum{}, maximum{};
    std::vector<uint16_t> coordinates;  // tightly packed xyz triples, not IEEE half
    uint32_t count() const { return uint32_t(coordinates.size() / 3); }
};
PointFrame readPointFrame(std::istream& input);
PointFrame loadPointFrame(const std::filesystem::path& path);
// CPU reference/debug positions. Production conversion is a Slang GPU dispatch.
std::vector<slang::Float4> decodePointFrame(const PointFrame& frame, float renderScale);
void validatePointFrame(const PointFrame& frame, float renderScale = 1);
struct QuantizationBounds {
    slang::Float4 minimum, range;
};
// Nonnegative axes match the original padded/clamped FP16 bounds. Negative axes
// use padded signed bounds so reconstruction retains their points.
QuantizationBounds pointQuantizationBounds(const std::vector<slang::Float4>& positions);
}  // namespace gvdb::samples::pointcloud

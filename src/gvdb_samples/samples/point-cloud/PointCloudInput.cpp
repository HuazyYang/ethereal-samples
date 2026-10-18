// SPDX-License-Identifier: Apache-2.0
#include "PointCloudInput.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
namespace gvdb::samples::pointcloud {
namespace {
uint32_t readWord(std::istream& input) {
    unsigned char b[4];
    input.read(reinterpret_cast<char*>(b), 4);
    if (!input)
        throw std::runtime_error("Truncated point-frame header");
    return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}
float readFloat(std::istream& input) {
    uint32_t word = readWord(input);
    float value;
    std::memcpy(&value, &word, 4);
    return value;
}
}  // namespace
void validatePointFrame(const PointFrame& frame, float scale) {
    if (frame.coordinates.empty() || frame.coordinates.size() % 3 ||
        frame.coordinates.size() / 3 > UINT32_MAX / 16)
        throw std::invalid_argument(
            "Point frame requires complete ushort3 positions within portable buffer limits");
    if (!std::isfinite(scale) || scale <= 0)
        throw std::invalid_argument("Point render scale must be finite and positive");
    const float lo[] = {frame.minimum.x, frame.minimum.y, frame.minimum.z},
                hi[] = {frame.maximum.x, frame.maximum.y, frame.maximum.z};
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(lo[axis]) || !std::isfinite(hi[axis]) || hi[axis] < lo[axis] ||
            !std::isfinite(hi[axis] - lo[axis]))
            throw std::invalid_argument("Point-frame bounds must be finite and ordered");
        if (!std::isfinite(lo[axis] * scale) || !std::isfinite(hi[axis] * scale) ||
            std::abs(lo[axis] * scale) > 4000000 || std::abs(hi[axis] * scale) > 4000000)
            throw std::invalid_argument("Scaled point frame exceeds the exact half-voxel coordinate domain");
    }
}
PointFrame readPointFrame(std::istream& input) {
    const uint32_t count = readWord(input);
    if (!count || count > UINT32_MAX / 16)
        throw std::invalid_argument("Invalid point-frame count");
    PointFrame frame;
    frame.minimum = {readFloat(input), readFloat(input), readFloat(input), 0};
    frame.maximum = {readFloat(input), readFloat(input), readFloat(input), 0};
    // Validate header bounds before allocating the full payload.
    frame.coordinates.resize(3);
    validatePointFrame(frame);
    const auto payload = input.tellg();
    if (payload != std::istream::pos_type(-1)) {
        input.seekg(0, std::ios::end);
        const auto end = input.tellg();
        if (end != std::istream::pos_type(-1) && uint64_t(end - payload) < uint64_t(count) * 6)
            throw std::runtime_error("Truncated ushort3 point-frame payload");
        input.clear();
        input.seekg(payload);
    }
    input.clear();  // tellg may fail on a valid non-seekable stream.
    frame.coordinates.resize(size_t(count) * 3);
    // Decode byte order explicitly, including payloads with an odd point count.
    std::vector<unsigned char> bytes(size_t(count) * 6);
    input.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!input)
        throw std::runtime_error("Truncated ushort3 point-frame payload");
    for (size_t i = 0; i < frame.coordinates.size(); ++i)
        frame.coordinates[i] = uint16_t(bytes[2 * i]) | (uint16_t(bytes[2 * i + 1]) << 8);
    return frame;
}
PointFrame loadPointFrame(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot open point frame: " + path.string());
    return readPointFrame(input);
}
std::vector<slang::Float4> decodePointFrame(const PointFrame& frame, float scale) {
    validatePointFrame(frame, scale);
    std::vector<slang::Float4> positions(frame.count());
    const float lo[] = {frame.minimum.x, frame.minimum.y, frame.minimum.z};
    const float delta[] = {(frame.maximum.x - frame.minimum.x) / 65535.f,
                           (frame.maximum.y - frame.minimum.y) / 65535.f,
                           (frame.maximum.z - frame.minimum.z) / 65535.f};
    for (size_t i = 0; i < positions.size(); ++i) {
        float value[3];
        for (int a = 0; a < 3; ++a)
            value[a] = (lo[a] + float(frame.coordinates[i * 3 + a]) * delta[a]) * scale;
        positions[i] = {value[0], value[1], value[2], 1};
    }
    return positions;
}
QuantizationBounds pointQuantizationBounds(const std::vector<slang::Float4>& positions) {
    if (positions.empty())
        throw std::invalid_argument("Point quantization requires nonempty input");
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (const auto& p : positions) {
        const float v[] = {p.x, p.y, p.z};
        for (int a = 0; a < 3; ++a) {
            if (!std::isfinite(v[a]) || std::abs(v[a]) > 4000000)
                throw std::invalid_argument("Point coordinates exceed the finite half-voxel domain");
            lo[a] = std::min(lo[a], v[a]);
            hi[a] = std::max(hi[a], v[a]);
        }
    }
    float range[3];
    for (int a = 0; a < 3; ++a) {
        lo[a] = lo[a] < 0 ? lo[a] - 1 : std::max(0.f, lo[a] - 1);
        hi[a] += 2;
        range[a] = hi[a] - lo[a];
    }
    return {
        {lo[0],    lo[1],    lo[2],    0},
        {range[0], range[1], range[2], 0}
    };
}
}  // namespace gvdb::samples::pointcloud

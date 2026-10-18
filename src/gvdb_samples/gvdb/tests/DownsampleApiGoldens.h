// SPDX-License-Identifier: Apache-2.0
// Independent constants: no production endpoint or summation helper is used.
#pragma once
#include "GVDBNvrhiDownsampleApi.h"
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
namespace downsample_fixture {
namespace g = gvdb::portable;
inline uint32_t bits(float value) {
    uint32_t result;
    std::memcpy(&result, &value, 4);
    return result;
}
inline float value(uint32_t bits) {
    float result;
    std::memcpy(&result, &bits, 4);
    return result;
}
struct Expected {
    enum Kind {
        Exact,
        NaN,
        PositiveInfinity,
        NegativeInfinity
    } kind = Exact;
    uint32_t bitPattern = 0;
    Expected(float v)
        : kind(Exact), bitPattern(bits(v)) {}
    Expected(Kind k)
        : kind(k) {}
};
struct Golden {
    std::string name;
    g::UInt3 sourceResolution{8, 1, 1}, destinationResolution{3, 1, 1};
    g::Float4 extent{8, 2, 2, 0};
    std::array<g::Float4, 3> rows{
        {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}
    };
    g::Float4 remap{0, 1, 0, 1};
    std::vector<float> source{0, 1, 2, 3, 4, 5, 6, 7};
    std::vector<Expected> expected;
    g::DownsampleRequestDesc request() const {
        g::DownsampleRequestDesc result;
        result.sourceData = source.data();
        result.sourceBytes = source.size() * sizeof(float);
        result.sourceResolution = sourceResolution;
        result.destinationResolution = destinationResolution;
        result.destinationExtent = extent;
        result.remap = remap;
        for (size_t i = 0; i < 3; ++i)
            result.transformRows[i] = rows[i];
        return result;
    }
    uint64_t inputBytes() const { return source.size() * sizeof(float); }
    uint64_t conservativeVisits() const {
        return uint64_t(sourceResolution.x) * sourceResolution.y * sourceResolution.z *
               destinationResolution.x * destinationResolution.y * destinationResolution.z;
    }
};
inline std::vector<Golden> goldens() {
    std::vector<Golden> all;
    Golden c;
    c.name = "res-plus-one inclusive regions";
    c.expected = {.5f, 2.5f, 4.5f};
    all.push_back(c);
    c.extent.x = 16;
    c.name = "upper boundary clamp";
    c.expected = {1.5f, 5.5f, 7.f};
    all.push_back(c);
    c.extent.x = 8;
    c.rows[0] = {-1, 0, 0, 7};
    c.name = "reflection keeps endpoint order";
    c.expected = {7.f, 5.f, 3.f};
    all.push_back(c);
    c.rows[0] = {1, 0, 0, -.75f};
    c.name = "truncate negative fractional endpoints toward zero";
    c.expected = {0.f, 1.5f, 3.5f};
    all.push_back(c);
    c.rows[0].w = -100;
    c.name = "fully negative clamp has a sample";
    c.expected = {0.f, 0.f, 0.f};
    all.push_back(c);
    c.rows[0].w = 100;
    c.name = "fully positive clamp has a sample";
    c.expected = {7.f, 7.f, 7.f};
    all.push_back(c);
    c.rows[0].w = 0;
    c.extent.x = 1;
    c.name = "tiny reversed pre-clamp endpoint";
    c.expected = {0.f, 0.f, 0.f};
    all.push_back(c);
    c = {};
    c.name = "shear transforms two endpoints, not eight corners";
    c.sourceResolution = {3, 2, 1};
    c.destinationResolution = {1, 1, 1};
    c.extent = {4, 4, 2, 0};
    c.rows[0] = {1, -1, 0, 1};
    c.source = {0, 1, 4, 10, 11, 14};
    c.expected = {6.f};
    all.push_back(c);
    c = {};
    c.name = "axis swap and affine row convention";
    c.sourceResolution = {3, 2, 1};
    c.destinationResolution = {2, 1, 1};
    c.extent = {6, 4, 2, 0};
    c.rows[0] = {0, 1, 0, 0};
    c.rows[1] = {1, 0, 0, 0};
    c.source = {0, 1, 2, 10, 11, 12};
    c.expected = {5.5f, 10.5f};
    all.push_back(c);
    c = {};
    c.name = "destination is X-fast through Y/Z planes";
    c.sourceResolution = {4, 4, 2};
    c.destinationResolution = {2, 2, 2};
    c.extent = {6, 6, 6, 0};
    c.source.clear();
    for (uint32_t z = 0; z < 2; ++z)
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 4; ++x)
                c.source.push_back(float(x + 10 * y + 100 * z));
    c.expected = {55.5f, 57.5f, 75.5f, 77.5f, 105.5f, 107.5f, 125.5f, 127.5f};
    all.push_back(c);
    c = {};
    // 6 is 0x40c00000. Dividing its immediate binary32 neighbors by 3
    // rounds to the immediate neighbors of 2: 0x3fffffff/0x40000000/0x40000001.
    // Inclusive upper endpoints therefore select [0,0],[1,2] below the
    // boundary and [0,1],[2,3] at or above it. These explicit samples reject
    // an epsilon-based integer snap. Reflection separately makes the genuine
    // successor observable: trunc(4 - nextafter(2,+inf)) is 1, not 2.
    for (int delta = -1; delta <= 1; ++delta) {
        c = {};
        c.name = "endpoint 6/3 immediate neighbor delta=" + std::to_string(delta);
        c.destinationResolution = {2, 1, 1};
        c.extent.x = value(uint32_t(int64_t(0x40c00000) + delta));
        c.expected = delta < 0 ? std::vector<Expected>{0.f, 1.5f} : std::vector<Expected>{.5f, 2.5f};
        all.push_back(c);
        c.name = "reflected endpoint 6/3 immediate neighbor delta=" + std::to_string(delta);
        c.rows[0] = {-1, 0, 0, 4};
        c.expected = delta > 0 ? std::vector<Expected>{4.f, 1.f} : std::vector<Expected>{4.f, 2.f};
        all.push_back(c);
    }
    c = {};
    c.name = "X-fast ordered Float32 accumulation";
    c.sourceResolution = {2, 2, 1};
    c.destinationResolution = {1, 1, 1};
    c.extent = {4, 4, 2, 0};
    c.source = {16777216.f, 1, -16777216.f, 3};
    c.expected = {.75f};
    all.push_back(c);
    c.name = "remap each sample before float accumulation";
    c.remap = {0, 1, 1, 3};
    c.expected = {2.75f};
    all.push_back(c);
    for (auto pair : {
             std::pair<uint32_t, float>{105, 5.25f},
              {120, 6.f  }
    })
        for (int delta = -1; delta <= 1; ++delta) {
            c = {};
            c.name = "strict .05 threshold " + std::to_string(pair.first) + " delta=" + std::to_string(delta);
            c.sourceResolution = {pair.first, 1, 1};
            c.destinationResolution = {1, 1, 1};
            c.extent = {float(pair.first * 2), 2, 2, 0};
            c.source.assign(pair.first, 0);
            c.source[0] = value(uint32_t(int64_t(bits(pair.second)) + delta));
            c.expected = {value(uint32_t(int64_t(0x3d4ccccd) + delta))};
            all.push_back(c);
        }
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN(),
                maximum = std::numeric_limits<float>::max();
    const std::array<std::array<float, 2>, 6> input{
        {{nan, 1}, {inf, 1}, {-inf, 1}, {inf, -inf}, {maximum, maximum}, {-maximum, -maximum}}
    };
    const Expected::Kind kind[] = {Expected::NaN, Expected::PositiveInfinity, Expected::NegativeInfinity,
                                   Expected::NaN, Expected::PositiveInfinity, Expected::NegativeInfinity};
    for (size_t i = 0; i < input.size(); ++i) {
        c = {};
        c.name = "IEEE source and accumulation class " + std::to_string(i);
        c.sourceResolution = {2, 1, 1};
        c.destinationResolution = {1, 1, 1};
        c.extent = {4, 2, 2, 0};
        c.source = {input[i][0], input[i][1]};
        c.expected = {Expected(kind[i])};
        all.push_back(c);
    }
    c = {};
    c.name = "unselected NaN cannot contaminate finite result";
    c.sourceResolution = {2, 1, 1};
    c.destinationResolution = {1, 1, 1};
    c.extent = {2, 2, 2, 0};
    c.source = {3, nan};
    c.expected = {3.f};
    all.push_back(c);
    // Repeat truncation with a genuinely different storage axis.
    c = {};
    c.name = "negative fractional Y endpoints";
    c.sourceResolution = {1, 8, 1};
    c.destinationResolution = {1, 3, 1};
    c.extent = {2, 8, 2, 0};
    c.rows[1].w = -.75f;
    c.expected = {0.f, 1.5f, 3.5f};
    all.push_back(c);
    c = {};
    c.name = "negative fractional Z endpoints";
    c.sourceResolution = {1, 1, 8};
    c.destinationResolution = {1, 1, 3};
    c.extent = {2, 2, 8, 0};
    c.rows[2].w = -.75f;
    c.expected = {0.f, 1.5f, 3.5f};
    all.push_back(c);
    return all;
}
}  // namespace downsample_fixture

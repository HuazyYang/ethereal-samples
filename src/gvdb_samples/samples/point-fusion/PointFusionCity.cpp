// SPDX-License-Identifier: Apache-2.0
#include "PointFusionCity.h"
#include <donut/core/math/math.h>
#include <cmath>
#include <random>
#include <stdexcept>
namespace gvdb::samples {
namespace {
namespace dm = donut::math;
slang::Float4 f4(dm::float3 v) { return {v.x, v.y, v.z, 0}; }
uint32_t pack(float r, float g, float b, float a) {
    return (uint32_t(r * 255) & 255u) | ((uint32_t(g * 255) & 255u) << 8) | ((uint32_t(b * 255) & 255u) << 16) |
           ((uint32_t(a * 255) & 255u) << 24);
}
class CityBuilder {
    std::mt19937 m_random{1043};
    float random(float low, float high) {
        return low + (high - low) * std::uniform_real_distribution<float>(0, 1)(m_random);
    }
    static dm::float3 location(dm::float3 block, float extent, dm::float3 dimension, dm::float3& size) {
        switch (int(block.y)) {
            case 0:
                size = {dimension.x, dimension.z, dimension.y};
                return {block.x * extent, 0, 0};
            case 1:
                size = {dimension.y, dimension.z, dimension.x};
                return {extent - dimension.y, 0, block.x * extent};
            case 2:
                size = {dimension.x, dimension.z, dimension.y};
                return {(1 - block.x) * extent - dimension.x, 0, extent - dimension.y};
            case 3:
                size = {dimension.y, dimension.z, dimension.x};
                return {0, 0, (1 - block.x) * extent - dimension.x};
            default:
                return dm::float3::zero();
        }
    }
    void building(dm::float3& block, dm::float3& position, dm::float3& size, float distance) {
        const float height = 3 + 200 * std::exp(distance / -200);
        // Keep the original expression and its host standard-library RNG behavior.
        dm::float3 dimension(random(5, 20), random(10, 40), random(3, height));
        if (block.x + dimension.x / 100 > 1)
            dimension.x = (1 - block.x) * 100;
        position = location(block, 100, dimension, size);
        block.x += dimension.x / 100;
        if (block.x >= 1) {
            block.x = dimension.y / 100;
            block.y++;
        }
    }

 public:
    PointFusionCity make() {
        PointFusionCity city;
        const float lane = 7.4f, curb = 3.7f;
        city.blockPitch = 100 + lane * 2 + curb * 2;
        const dm::float3 center(10 * city.blockPitch * .5f, 0, 10 * city.blockPitch * .5f);
        city.maximum = {10 * city.blockPitch * 5, 0, 10 * city.blockPitch * 5, 0};
        city.initialEye = {2 * city.blockPitch * 5, 20, 2 * city.blockPitch * 5, 0};
        city.boxes.reserve(10000);
        auto add = [&](dm::float3 position, dm::float3 size, uint32_t color) {
            const auto p = position * 5.f, s = size * 5.f;
            city.boxes.push_back({
                f4(p), f4(p + s), {color, 0, 0, 0}
            });
        };
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 10; ++x) {
                const dm::float3 blockPosition(x * city.blockPitch + lane + curb, 0,
                                               y * city.blockPitch + lane + curb);
                dm::float3 block = dm::float3::zero(), position, size, unused;
                int count = 0;
                while (block.y < 4 && count < 100) {
                    const float distance =
                        dm::length(center - (blockPosition + location(block, 100, dm::float3::zero(), unused)));
                    building(block, position, size, distance);
                    const uint32_t color = pack(random(0, .9f), random(0, .5f), 0, 1);
                    add(blockPosition + position, size, color);
                    ++count;
                }
                add(blockPosition - dm::float3(curb, 0, curb), {100 + curb * 2, .25f, 100 + curb * 2},
                    pack(.3f, .3f, .3f, 1));
                add(blockPosition - dm::float3(lane + curb, 0, lane + curb),
                    {100 + (lane + curb) * 2, .01f, 100 + (lane + curb) * 2}, pack(.5f, .5f, .5f, 1));
            }
        return city;
    }
};
}  // namespace
PointFusionCity makePointFusionCity() { return CityBuilder{}.make(); }
ScanParameters cityScanParameters(const PointFusionCity& city, slang::Float4 eye, float yawDegrees,
                                  float pitchDegrees, uint32_t width, uint32_t height, bool jitter) {
    if (!width || !height || !std::isfinite(yawDegrees) || !std::isfinite(pitchDegrees))
        throw std::invalid_argument("Invalid city scanner camera");
    constexpr float radians = 3.14159265358979323846f / 180;
    const float yaw = yawDegrees * radians, pitch = pitchDegrees * radians;
    const dm::float3 back(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
    const dm::float3 worldUp = std::abs(back.y) > .999f ? dm::float3(0, 0, 1) : dm::float3(0, 1, 0);
    const auto right = dm::normalize(dm::cross(worldUp, back)), up = dm::normalize(dm::cross(back, right));
    const float vertical = float(height) / width;
    ScanParameters p{};
    p.originAndMaximum = {eye.x, eye.y, eye.z, city.blockPitch * 4 * city.indexScale};
    p.direction00 = f4(-back - right + up * vertical);
    p.directionDx = f4(right * 2.f);
    p.directionDy = f4(up * (-2 * vertical));
    p.cityMinimum = city.minimum;
    p.cityMaximum = city.maximum;
    p.options.x = jitter ? 1 : 0;
    return p;
}
}  // namespace gvdb::samples

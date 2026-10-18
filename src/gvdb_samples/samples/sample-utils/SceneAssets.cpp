// SPDX-License-Identifier: Apache-2.0
#include "SceneAssets.h"
#include "ObjLoader.h"
#include <cmath>
#include <memory>
#include <stb_image.h>
#include <stdexcept>
namespace gvdb::sample
{
SceneMesh loadSceneObj(const std::filesystem::path &path, float scale, dm::float3 offset)
{
    if (!std::isfinite(scale) || scale <= 0 || !std::isfinite(offset.x) || !std::isfinite(offset.y) ||
        !std::isfinite(offset.z))
        throw std::invalid_argument("OBJ scale/offset must be finite and scale positive");
    auto fs = MAKE_RC_OBJ_PTR(donut::vfs::NativeFileSystem);
    auto source = SampleUtils::ObjLoader()(fs, path.generic_string().c_str());
    if (source.positions.empty() || source.indices.empty() || source.positions.size() > UINT32_MAX / 16 ||
        source.indices.size() > UINT32_MAX / 12)
        throw std::runtime_error("Invalid or empty scene OBJ: " + path.string());
    for (const auto &t : source.indices)
        if (t.x < 0 || t.y < 0 || t.z < 0 || size_t(t.x) >= source.positions.size() ||
            size_t(t.y) >= source.positions.size() || size_t(t.z) >= source.positions.size())
            throw std::runtime_error("OBJ triangle index exceeds vertex count");
    if (source.normals.size() != source.positions.size())
    {
        source.normals.assign(source.positions.size(), dm::float3::zero());
        for (const auto &t : source.indices)
        {
            const auto n =
                dm::cross(source.positions[t.y] - source.positions[t.x], source.positions[t.z] - source.positions[t.x]);
            source.normals[t.x] += n;
            source.normals[t.y] += n;
            source.normals[t.z] += n;
        }
        for (auto &n : source.normals)
            n = dm::lengthSquared(n) > 0 ? dm::normalize(n) : dm::float3(0, 1, 0);
    }
    SceneMesh result;
    for (size_t i = 0; i < source.positions.size(); ++i)
    {
        const auto p = source.positions[i] * scale + offset;
        const auto normal = source.normals[i];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(normal.x) ||
            !std::isfinite(normal.y) || !std::isfinite(normal.z))
            throw std::runtime_error("Nonfinite OBJ position or normal");
        const auto n = dm::snorm8ToVector<3>(dm::vectorToSnorm8(normal));
        result.positions.push_back({p.x, p.y, p.z, 1});
        result.normals.push_back({n.x, n.y, n.z, 0});
    }
    for (const auto &t : source.indices)
    {
        result.indices.push_back(uint32_t(t.x));
        result.indices.push_back(uint32_t(t.y));
        result.indices.push_back(uint32_t(t.z));
    }
    return result;
}
SceneEnvironment loadSceneEnvironment(const std::filesystem::path &path)
{
    int width = 0, height = 0, channels = 0;
    using Pixels = std::unique_ptr<stbi_uc, decltype(&stbi_image_free)>;
    Pixels pixels(stbi_load(path.string().c_str(), &width, &height, &channels, 4), stbi_image_free);
    if (!pixels || width < 1 || height < 1 || width > 16384 || height > 16384)
        throw std::runtime_error("Cannot load supported scene environment: " + path.string());
    SceneEnvironment result;
    result.width = uint32_t(width);
    result.height = uint32_t(height);
    result.rgba.assign(pixels.get(), pixels.get() + size_t(width) * height * 4);
    return result;
}
} // namespace gvdb::sample

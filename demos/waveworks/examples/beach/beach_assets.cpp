#include "beach_assets.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace {
uint32_t Hash(uint32_t v) {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    return v ^ (v >> 16);
}
float Unit(uint32_t v) {
    return float(Hash(v) & 0xffffffu) / 16777216.f;
}
void Rotate(float& u, float& v, uint32_t q) {
    float a = u, b = v;
    if (q == 1) {
        u = -b;
        v = a;
    }
    if (q == 2) {
        u = -a;
        v = -b;
    }
    if (q == 3) {
        u = b;
        v = -a;
    }
}
template <class T>
bool Read(std::ifstream& file, T& data) {
    return bool(file.read(reinterpret_cast<char*>(&data), sizeof(data)));
}
float BedHeight(const BeachBathymetry& bed, float x, float z) {
    const auto& m = bed.metadata;
    float u = (x - m.originX) / m.averageStepX;
    float v = (z - m.originZ) / m.averageStepZ;
    int i = int(std::floor(u)), j = int(std::floor(v));
    if (i < 0 || j < 0 || i + 1 >= int(m.width) || j + 1 >= int(m.height))
        return -10000.f;
    float a = u - i, b = v - j;
    size_t k = size_t(j) * m.width + i;
    float h00 = bed.elevations[k], h10 = bed.elevations[k + 1];
    float h01 = bed.elevations[k + m.width], h11 = bed.elevations[k + m.width + 1];
    return a + b <= 1 ? h00 + a * (h10 - h00) + b * (h01 - h00)
                      : h11 + (1 - a) * (h01 - h11) + (1 - b) * (h10 - h11);
}
}  // namespace

bool BuildBeachGroundVisibility(const BeachBathymetry& bed, gfsdk_float4 bounds,
                                uint32_t width, uint32_t height,
                                std::vector<float>& values) {
    values.clear();
    if (!bed.IsValid() || width == 0 || height == 0 || width > 1024 || height > 1024 ||
        !std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.z) ||
        !std::isfinite(bounds.w) || bounds.z <= 0 || bounds.w <= 0)
        return false;
    const auto& m = bed.metadata;
    if (bounds.x < m.minimumX + 1 || bounds.y < m.minimumZ + 1 ||
        bounds.x + bounds.z > m.maximumX - 1 || bounds.y + bounds.w > m.maximumZ - 1)
        return false;
    if (std::any_of(bed.elevations.begin(), bed.elevations.end(),
                    [](float value) { return !std::isfinite(value); }))
        return false;
    constexpr float pi = 3.14159265359f;
    constexpr uint32_t directions = 16;
    gfsdk_float2 azimuth[directions];
    for (uint32_t i = 0; i < directions; ++i) {
        float phi = (i + .5f) * (2.f * pi / directions);
        azimuth[i] = {std::cos(phi), std::sin(phi)};
    }
    values.resize(size_t(width) * height);
    for (uint32_t j = 0; j < height; ++j)
        for (uint32_t i = 0; i < width; ++i) {
            const float x = bounds.x + (i + .5f) * bounds.z / width;
            const float z = bounds.y + (j + .5f) * bounds.w / height;
            const float y = BedHeight(bed, x, z);
            const auto n = normalize(
                gfsdk_float3{BedHeight(bed, x - .5f, z) - BedHeight(bed, x + .5f, z), 1.f,
                             BedHeight(bed, x, z - .5f) - BedHeight(bed, x, z + .5f)});
            float integral = 0.f;
            for (const auto& direction : azimuth) {
                const float horizontal = n.x * direction.x + n.z * direction.y;
                float horizon = std::atan2(-horizontal, n.y);
                for (float radius :
                     {.7f, 1.4f, 2.4f, 4.f, 6.f, 9.f, 13.f, 19.f, 28.f, 42.f}) {
                    const float h =
                        BedHeight(bed, x + direction.x * radius, z + direction.y * radius);
                    horizon = std::max(horizon, std::atan2(h - y - .025f, radius));
                }
                // Integral of max(N.L,0) dOmega over elevations above the
                // measured horizon. Unlike an upward-only AO approximation,
                // this gives unit visibility on an unobstructed inclined plane.
                const float c = std::cos(horizon);
                integral += horizontal * (pi * .25f - horizon * .5f -
                                          std::sin(2.f * horizon) * .25f) +
                            n.y * .5f * c * c;
            }
            values[size_t(j) * width + i] =
                std::clamp(integral * 2.f / directions, 0.f, 1.f);
        }
    return true;
}

bool BeachScalarTexture::IsValid() const {
    return width >= 2 && height >= 2 && width <= 2048 && height <= 2048 &&
           pixels.size() == size_t(width) * height;
}
bool BeachScalarTexture::Load(const std::string& path) {
    *this = {};
    std::ifstream file(path, std::ios::binary);
    char magic[4];
    if (!Read(file, magic) || std::memcmp(magic, "BH01", 4) || !Read(file, width) ||
        !Read(file, height) || width < 2 || height < 2 || width > 2048 || height > 2048) {
        *this = {};
        return false;
    }
    pixels.resize(size_t(width) * height);
    if (!file.read(reinterpret_cast<char*>(pixels.data()), pixels.size() * sizeof(float)) ||
        file.peek() != std::char_traits<char>::eof() ||
        std::any_of(pixels.begin(), pixels.end(),
                    [](float p) { return !std::isfinite(p) || p < 0 || p > 1; })) {
        *this = {};
        return false;
    }
    return true;
}
float BeachScalarTexture::Sample(float u, float v) const {
    if (!IsValid() || !std::isfinite(u) || !std::isfinite(v))
        return 0;
    float x = (u - std::floor(u)) * width - .5f;
    float y = (v - std::floor(v)) * height - .5f;
    int i = int(std::floor(x)), j = int(std::floor(y));
    float a = x - i, b = y - j;
    auto p = [&](int dx, int dy) {
        return pixels[size_t((j + dy + int(height)) % int(height)) * width +
                      (i + dx + int(width)) % int(width)];
    };
    return (p(0, 0) * (1 - a) + p(1, 0) * a) * (1 - b) +
           (p(0, 1) * (1 - a) + p(1, 1) * a) * b;
}
float BeachScalarTexture::SampleStochastic(float u, float v) const {
    if (!std::isfinite(u) || !std::isfinite(v) || std::abs(u) > 1e6f || std::abs(v) > 1e6f)
        return 0;
    // Same patch transforms and weights as BeachTextureSampling.hlsli.
    float x = u - v * .577350269f, y = v * 1.154700538f;
    int ix = int(std::floor(x)), iy = int(std::floor(y));
    float a = x - ix, b = y - iy;
    int cx[3] = {ix, ix + 1, ix}, cy[3] = {iy, iy, iy + 1};
    float w[3] = {1 - a - b, a, b};
    if (a + b > 1) {
        cx[0] = ix + 1;
        cy[0] = iy + 1;
        cx[1] = ix;
        cy[1] = iy + 1;
        cx[2] = ix + 1;
        cy[2] = iy;
        w[0] = a + b - 1;
        w[1] = 1 - a;
        w[2] = 1 - b;
    }
    float value = 0, sum = 0;
    for (int k = 0; k < 3; ++k) {
        uint32_t h = Hash(uint32_t(cx[k]) ^ Hash(uint32_t(cy[k]) + 0x9e3779b9u));
        float pu = u, pv = v;
        Rotate(pu, pv, h & 3);
        pu += float(h & 65535u) / 65536.f;
        pv += float(Hash(h) & 65535u) / 65536.f;
        value += Sample(pu, pv) * w[k] * w[k];
        sum += w[k] * w[k];
    }
    return value / sum;
}
bool BeachGroundTextures::Load(const std::string& folder) {
    *this = {};
    return groundHeight.Load(folder + "/coastal_ground_height.bin") &&
           sandHeight.Load(folder + "/sand_height.bin") &&
           cover.Load(folder + "/coastal_ground_cover.bin");
}
bool BeachGroundTextures::IsValid() const {
    return groundHeight.IsValid() && sandHeight.IsValid() && cover.IsValid();
}
bool LoadBeachPropMeshes(const std::string& path, std::vector<BeachPropMesh>& meshes) {
    meshes.clear();
    std::ifstream file(path, std::ios::binary);
    char magic[4];
    uint32_t count = 0;
    if (!Read(file, magic) || std::memcmp(magic, "BM01", 4) || !Read(file, count) ||
        count == 0 || count > 64)
        return false;
    std::vector<BeachPropMesh> result(count);
    uint64_t totalBytes = 0;
    for (auto& mesh : result) {
        uint32_t nv = 0, ni = 0;
        if (!Read(file, nv) || !Read(file, ni) || !nv || !ni || ni % 3 || nv > 1000000 ||
            ni > 3000000)
            return false;
        totalBytes += uint64_t(nv) * sizeof(BeachVertex) + uint64_t(ni) * 4;
        if (totalBytes > 128 * 1024 * 1024)
            return false;
        mesh.vertices.resize(nv);
        mesh.indices.resize(ni);
        if (!file.read(reinterpret_cast<char*>(mesh.vertices.data()),
                       nv * sizeof(BeachVertex)) ||
            !file.read(reinterpret_cast<char*>(mesh.indices.data()), ni * 4))
            return false;
        for (const auto& v : mesh.vertices) {
            float n = v.nx * v.nx + v.ny * v.ny + v.nz * v.nz;
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
                !std::isfinite(v.u) || !std::isfinite(v.v) || !std::isfinite(n) ||
                std::abs(n - 1) > .02f || v.y < -.001f)
                return false;
            mesh.height = std::max(mesh.height, v.y);
        }
        if (mesh.height <= 0 || std::any_of(mesh.indices.begin(), mesh.indices.end(),
                                            [&](uint32_t i) { return i >= nv; }))
            return false;
    }
    if (file.peek() != std::char_traits<char>::eof())
        return false;
    meshes = std::move(result);
    return true;
}
bool BuildBeachPropInstances(const std::vector<BeachPropMesh>& meshes, uint32_t kind,
                             const BeachBathymetry& bed, const BeachGroundTextures& ground,
                             BeachPropInstances& instances) {
    instances.clear();
    if (meshes.empty() || kind > 2 || !bed.IsValid() || !ground.IsValid())
        return false;
    instances.resize(meshes.size());
    // Placement randomness changes only instance transforms, never surface
    // color or terrain height. Plant acceptance follows the photographed cover.
    float spacing = kind == 0 ? 10.5f : (kind == 1 ? .7f : 1.6f);
    uint32_t serial = 0;
    for (float z = -105; z < 205; z += spacing)
        for (float x = -80; x < 240; x += spacing, ++serial) {
            uint32_t h = Hash(serial ^ (kind + 1) * 0x9237a2u);
            float px = x + Unit(h) * spacing, pz = z + Unit(h + 1) * spacing;
            float y = BedHeight(bed, px, pz);
            if (y < 2.35f || y > 24.f)
                continue;
            float cover = ground.cover.SampleStochastic(px / BeachGroundScanSize,
                                                        pz / BeachGroundScanSize);
            if (Unit(h + 2) > (kind == 0 ? .15f + .25f * (1 - cover) : cover * .75f))
                continue;
            bool outcrop = false;
            if (kind == 0 && y > 3.f && Unit(h + 10) < .55f) {
                const float slopeX =
                    BedHeight(bed, px + .5f, pz) - BedHeight(bed, px - .5f, pz);
                const float slopeZ =
                    BedHeight(bed, px, pz + .5f) - BedHeight(bed, px, pz - .5f);
                outcrop = slopeX * slopeX + slopeZ * slopeZ > .035f;
            }
            // Partly buried groups expose broken rock silhouettes on slopes.
            // Assemble tufts from the distinct photographed leaves/stems;
            // each member keeps its own source mesh, UVs and rooted transform.
            uint32_t members = kind == 0 ? (outcrop ? 3 : 1) : (kind == 1 ? 8 : 12);
            for (uint32_t member = 0; member < members; ++member) {
                uint32_t seed = h + member * 173;
                uint32_t variant = Hash(seed + 3) % uint32_t(meshes.size());
                float scale = outcrop     ? .7f + Unit(seed + 4) * .7f
                              : kind == 0 ? .12f + Unit(seed + 4) * .42f
                                          : 1.2f + Unit(seed + 4) * 1.3f;
                float angle = Unit(seed + 5) * 6.283185307f;
                float height = meshes[variant].height * scale;
                float spread = outcrop ? 3.f : (kind ? .32f : 0.f);
                float rx = px + (Unit(seed + 6) - .5f) * spread;
                float rz = pz + (Unit(seed + 7) - .5f) * spread;
                float ry = BedHeight(bed, rx, rz);
                instances[variant].push_back(
                    {rx, ry - (kind == 0 ? height * (outcrop ? .68f : .48f) : .005f), rz,
                     scale, std::cos(angle), std::sin(angle), height,
                     kind == 0 ? 0.f : 1.f});
            }
        }
    // Individually placed, low foreground stones and exposed bluff rocks.
    // These are scene-space asset transforms, not pixels sampled from the reference.
    if (kind == 0) {
        const float placements[][4] = {{-61, -72, .45f, .4f}, {-58, -68, .28f, 2.1f},
                                       {-45, -85, .32f, 1.f}, {-13, -4, .65f, .7f},
                                       {8, 29, .50f, 1.8f},   {28, 42, .7f, 2.7f},
                                       {55, 30, .4f, 3.5f},   {121, -13, .25f, .2f}};
        for (size_t i = 0; i < sizeof(placements) / sizeof(placements[0]); ++i) {
            auto p = placements[i];
            size_t variant = i % meshes.size();
            float height = meshes[variant].height * p[2];
            float y = BedHeight(bed, p[0], p[1]);
            if (y < .4f)
                continue;
            instances[variant].push_back({p[0], y - height * .28f, p[1], p[2],
                                          std::cos(p[3]), std::sin(p[3]), height, 0});
        }
    }
    return true;
}

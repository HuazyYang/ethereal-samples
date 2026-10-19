// SPDX-License-Identifier: Apache-2.0
#include "PointCloudReconstruction.h"
#include "PointCloudScene.h"
#include <gvdb/GVDBSlangPointConversion.h>
#include "ExampleDevice.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <tuple>
namespace {
using namespace gvdb::samples::pointcloud;
using namespace gvdb::slang;
using Coordinate = std::tuple<int32_t, int32_t, int32_t>;
void require(bool value, const char* what) {
    if (!value)
        throw std::runtime_error(what);
}
void expectNear(float actual, float expected, float tolerance, const char* what) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
        throw std::runtime_error(std::string(what) + ": actual=" + std::to_string(actual) +
                                 " expected=" + std::to_string(expected));
}
std::map<Coordinate, float> reference(const PackedVolume& packed, const std::vector<Float4>& points,
                                      const ReconstructionSettings& settings,
                                      const std::vector<uint16_t>& codes) {
    float minimum[3] = {INFINITY, INFINITY, INFINITY}, maximum[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (const auto& p : points) {
        const float v[] = {p.x, p.y, p.z};
        for (int a = 0; a < 3; ++a) {
            minimum[a] = std::min(minimum[a], v[a]);
            maximum[a] = std::max(maximum[a], v[a]);
        }
    }
    float range[3];
    for (int a = 0; a < 3; ++a) {
        minimum[a] = minimum[a] < 0 ? minimum[a] - 1 : std::max(0.f, minimum[a] - 1);
        range[a] = maximum[a] + 2 - minimum[a];
    }
    std::vector<Float4> quantized;
    size_t componentIndex = 0;
    for (const auto& p : points) {
        const float original[] = {p.x, p.y, p.z};
        float decoded[3];
        for (int a = 0; a < 3; ++a) {
            const uint16_t q = codes.at(componentIndex++);
            decoded[a] = (float(q) / 65535.f) * range[a] + minimum[a];
        }
        quantized.push_back({decoded[0], decoded[1], decoded[2], 1});
    }
    std::map<Coordinate, float> values;
    const auto& level = packed.levels[0];
    for (uint32_t leaf = 0; leaf < level.nodes.z; ++leaf) {
        const auto* node = packed.nodes.data() + (level.nodes.x + size_t(leaf) * level.nodes.y) / 4;
        for (int z = 0; z < 16; ++z)
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x) {
                    const int32_t world[] = {int32_t(node[1]) + x, int32_t(node[2]) + y, int32_t(node[3]) + z};
                    float result = 3;
                    for (size_t j = 0; j < points.size(); ++j) {
                        const float original[] = {points[j].x, points[j].y, points[j].z};
                        bool member = true;
                        for (int a = 0; a < 3; ++a) {
                            const int cell = int(std::floor(double(world[a]) / 4));
                            member &= cell >= int(std::floor((original[a] - 1) / 4)) &&
                                      cell <= int(std::floor((original[a] + 2) / 4));
                        }
                        if (member) {
                            const auto& p = quantized[j];
                            const float dx = p.x - world[0], dy = p.y - world[1], dz = p.z - world[2];
                            result = std::min(result, std::sqrt(dx * dx + dy * dy + dz * dz) - 1);
                        }
                    }
                    values.emplace(Coordinate{world[0], world[1], world[2]}, result);
                }
    }
    const auto sample = [](const auto& map, int32_t x, int32_t y, int32_t z) {
        const auto found = map.find(Coordinate{x, y, z});
        return found == map.end() ? 3.f : found->second;
    };
    for (uint32_t pass = 0; pass < settings.smoothingIterations; ++pass) {
        auto next = values;
        for (auto& entry : next) {
            const auto [x, y, z] = entry.first;
            const float neighbors = sample(values, x - 1, y, z) + sample(values, x + 1, y, z) +
                                    sample(values, x, y - 1, z) + sample(values, x, y + 1, z) +
                                    sample(values, x, y, z - 1) + sample(values, x, y, z + 1);
            entry.second = (settings.smoothingCenterWeight * values.at(entry.first) + neighbors) /
                               (settings.smoothingCenterWeight + 6) +
                           settings.smoothingBias;
        }
        values = std::move(next);
    }
    return values;
}
void verify(ethereal::slang::ComputeRuntime& runtime, PointCloudReconstruction& reconstruction,
            const PointFrame& frame, const ReconstructionSettings& settings) {
    const auto expectedPoints = decodePointFrame(frame, settings.renderScale);
    const auto& actual = reconstruction.hostPositions();
    require(actual.size() == expectedPoints.size(), "Converted point count");
    for (size_t i = 0; i < actual.size(); ++i) {
        expectNear(actual[i].x, expectedPoints[i].x, 2e-5f, "Converted x");
        expectNear(actual[i].y, expectedPoints[i].y, 2e-5f, "Converted y");
        expectNear(actual[i].z, expectedPoints[i].z, 2e-5f, "Converted z");
        require(actual[i].w == 1, "Converted position validity");
    }
    // Validate compression separately. Shader division may round across one
    // ushort boundary; then test gathering/smoothing against those verified
    // codes without folding quantization error into the stencil tolerance.
    const auto bounds = reconstruction.quantization();
    const auto codes = reconstruction.quantizationCodes();
    const float minimum[] = {bounds.minimum.x, bounds.minimum.y, bounds.minimum.z};
    const float range[] = {bounds.range.x, bounds.range.y, bounds.range.z};
    int maximumCodeError = 0;
    for (size_t i = 0; i < actual.size(); ++i) {
        const float value[] = {actual[i].x, actual[i].y, actual[i].z};
        for (int a = 0; a < 3; ++a) {
            const int cpu = int((value[a] - minimum[a]) / range[a] * 65535.f);
            const int error = std::abs(int(codes[i * 3 + a]) - cpu);
            require(error <= 1, "Subcell quantization exceeds one ushort interval");
            maximumCodeError = std::max(maximumCodeError, error);
        }
    }
    const auto& topology = reconstruction.readTopology();
    require(topology.levels[0].nodeExtent.x == 16 && topology.volume.atlasBrickCountAndWidth.w == 18,
            "Legacy fromBranching(...,4) requires leaf16 and apron18");
    // Independently enumerate integer support voxels, then partition into legacy
    // leaves. This detects extra/missing bricks as well as wrong branching order.
    std::map<Coordinate, bool> expectedLeaves, actualLeaves;
    bool signedAxis[3]{};
    for (const auto& point : expectedPoints) {
        signedAxis[0] |= point.x < 0;
        signedAxis[1] |= point.y < 0;
        signedAxis[2] |= point.z < 0;
    }
    for (const auto& point : expectedPoints) {
        const float v[3]{point.x, point.y, point.z};
        int32_t base[3];
        for (int axis = 0; axis < 3; ++axis)
            base[axis] = signedAxis[axis] ? int32_t(std::floor(v[axis])) : int32_t(v[axis]);
        for (int z = base[2] - 2; z <= base[2] + 4; ++z)
            for (int y = base[1] - 2; y <= base[1] + 4; ++y)
                for (int x = base[0] - 2; x <= base[0] + 4; ++x) {
                    const int q[3]{x, y, z};
                    int32_t origin[3];
                    for (int axis = 0; axis < 3; ++axis)
                        origin[axis] =
                            (signedAxis[axis] ? int32_t(std::floor(double(q[axis]) / 16)) : q[axis] / 16) * 16;
                    expectedLeaves[{origin[0], origin[1], origin[2]}] = true;
                }
    }
    for (uint32_t i = 0; i < topology.levels[0].nodes.z; ++i) {
        const auto* node =
            topology.nodes.data() + (topology.levels[0].nodes.x + size_t(i) * topology.levels[0].nodes.y) / 4;
        actualLeaves[{int32_t(node[1]), int32_t(node[2]), int32_t(node[3])}] = true;
    }
    require(actualLeaves == expectedLeaves, "Independent legacy support topology");
    const auto expected = reference(reconstruction.readTopology(), expectedPoints, settings, codes);
    std::cout << "Normalized ushort3 quantization maximum code difference=" << maximumCodeError << '\n';
    const auto atlas = reconstruction.readAtlas();
    const auto& packed = reconstruction.readTopology();
    const auto resolution = packed.volume.atlasResolutionAndApron;
    const auto& level = packed.levels[0];
    uint64_t checked = 0;
    for (uint32_t leaf = 0; leaf < level.nodes.z; ++leaf) {
        const auto* node = packed.nodes.data() + (level.nodes.x + size_t(leaf) * level.nodes.y) / 4;
        for (int z = -1; z <= 16; ++z)
            for (int y = -1; y <= 16; ++y)
                for (int x = -1; x <= 16; ++x) {
                    const auto key =
                        Coordinate{int32_t(node[1]) + x, int32_t(node[2]) + y, int32_t(node[3]) + z};
                    const auto found = expected.find(key);
                    const float value = found == expected.end() ? 3.f : found->second;
                    const size_t index =
                        (size_t(int32_t(node[6]) + z) * resolution.y + uint32_t(int32_t(node[5]) + y)) *
                            resolution.x +
                        uint32_t(int32_t(node[4]) + x);
                    expectNear(atlas.at(index), value, 5e-5f,
                               "Independent subcell level-set/smoothing/apron reference");
                    ++checked;
                }
    }
    const auto lines = reconstruction.pointLines();
    require(lines.size() == actual.size(), "Small point overlay count");
    expectNear(lines[0].end.x - lines[0].start.x, .5f, 1e-6f, "Point line glyph extent");
    expectNear(lines[0].color.z, actual[0].z / 256, 1e-6f, "Point line color");
    std::cout << "Point-cloud reference: " << actual.size() << " points, " << level.nodes.z << " leaves, "
              << checked << " atlas/apron voxels\n";
}
// Independent source-file reference at eight occupied voxels. Evaluate all
// original points for each stencil coordinate; do not reuse GPU bins.
void verifyAssetSamples(PointCloudReconstruction& reconstruction, const PointFrame& frame,
                        const ReconstructionSettings& settings, const std::vector<float>& atlas) {
    require(settings.smoothingIterations == 1, "Real reference expects teapot's one smoothing pass");
    const auto original = decodePointFrame(frame, settings.renderScale);
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (const auto& p : original) {
        const float v[] = {p.x, p.y, p.z};
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], v[a]);
            hi[a] = std::max(hi[a], v[a]);
        }
    }
    float range[3];
    for (int a = 0; a < 3; ++a) {
        lo[a] = std::max(0.f, lo[a] - 1);
        range[a] = hi[a] + 2 - lo[a];
    }
    std::vector<Float4> decoded;
    decoded.reserve(original.size());
    for (const auto& p : original) {
        const float v[] = {p.x, p.y, p.z};
        float result[3];
        for (int a = 0; a < 3; ++a) {
            const uint16_t q = uint16_t((v[a] - lo[a]) / range[a] * 65535.f);
            result[a] = float(q) / 65535.f * range[a] + lo[a];
        }
        decoded.push_back({result[0], result[1], result[2], 1});
    }
    std::vector<Coordinate> selected;
    std::map<Coordinate, float> gathered;
    for (size_t i = 0; i < 8; ++i) {
        const auto& p = original[i * (original.size() - 1) / 7];
        const int32_t x = int32_t(std::floor(p.x)), y = int32_t(std::floor(p.y)), z = int32_t(std::floor(p.z));
        selected.emplace_back(x, y, z);
        for (const Coordinate& key : {
                 Coordinate{x,     y,     z    },
                 Coordinate{x - 1, y,     z    },
                 Coordinate{x + 1, y,     z    },
                 Coordinate{x,     y - 1, z    },
                 Coordinate{x,     y + 1, z    },
                 Coordinate{x,     y,     z - 1},
                 Coordinate{x,     y,     z + 1}
        })
            gathered.emplace(key, 3.f);
    }
    for (auto& query : gathered) {
        const auto [x, y, z] = query.first;
        const int cx = x / 4, cy = y / 4, cz = z / 4;
        for (size_t j = 0; j < original.size(); ++j) {
            const auto& p = original[j];
            if (cx < int(std::floor((p.x - 1) / 4)) || cx > int(std::floor((p.x + 2) / 4)) ||
                cy < int(std::floor((p.y - 1) / 4)) || cy > int(std::floor((p.y + 2) / 4)) ||
                cz < int(std::floor((p.z - 1) / 4)) || cz > int(std::floor((p.z + 2) / 4)))
                continue;
            const auto& q = decoded[j];
            const float dx = q.x - x, dy = q.y - y, dz = q.z - z;
            query.second = std::min(query.second, std::sqrt(dx * dx + dy * dy + dz * dz) - 1);
        }
    }
    const auto& packed = reconstruction.readTopology();
    const auto& level = packed.levels[0];
    const auto resolution = packed.volume.atlasResolutionAndApron;
    std::map<Coordinate, const uint32_t*> leafNodes;
    for (uint32_t leaf = 0; leaf < level.nodes.z; ++leaf) {
        const auto* node = packed.nodes.data() + (level.nodes.x + size_t(leaf) * level.nodes.y) / 4;
        leafNodes.emplace(Coordinate{int32_t(node[1]), int32_t(node[2]), int32_t(node[3])}, node);
    }
    const auto sample = [&](int32_t x, int32_t y, int32_t z) {
        const auto found = leafNodes.find(Coordinate{int32_t(std::floor(double(x) / 16)) * 16,
                                                     int32_t(std::floor(double(y) / 16)) * 16,
                                                     int32_t(std::floor(double(z) / 16)) * 16});
        return found == leafNodes.end() ? 3.f : gathered.at(Coordinate{x, y, z});
    };
    // One ushort quantization interval bounds backend rounding at large world
    // coordinates. Analytic fixtures use a tighter 5e-5 absolute bound.
    const float tolerance =
        std::sqrt(range[0] * range[0] + range[1] * range[1] + range[2] * range[2]) / 65535.f + .002f;
    float maximumError = 0;
    for (const auto& key : selected) {
        const auto [x, y, z] = key;
        const auto* node = leafNodes.at(Coordinate{(x / 16) * 16, (y / 16) * 16, (z / 16) * 16});
        const float neighbors = sample(x - 1, y, z) + sample(x + 1, y, z) + sample(x, y - 1, z) +
                                sample(x, y + 1, z) + sample(x, y, z - 1) + sample(x, y, z + 1);
        const float expected = (settings.smoothingCenterWeight * sample(x, y, z) + neighbors) /
                                   (settings.smoothingCenterWeight + 6) +
                               settings.smoothingBias;
        const size_t index =
            (size_t(node[6] + z - int32_t(node[3])) * resolution.y + node[5] + y - int32_t(node[2])) *
                resolution.x +
            node[4] + x - int32_t(node[1]);
        const float actual = atlas.at(index);
        maximumError = std::max(maximumError, std::abs(actual - expected));
        expectNear(actual, expected, tolerance, "Real asset independent gather/smoothing reference");
    }
    std::cout << "Real point-cloud CPU reference: " << selected.size()
              << " occupied voxels, max error=" << maximumError << ", tolerance=" << tolerance << '\n';
}
}  // namespace
int main(int argc, const char** argv) {
    try {
        ethereal::slang::ExampleDevice device(argc, argv);
        ethereal::slang::ComputeRuntime runtime(device.get(), device.shaderPath());
        bool asset = false;
        std::filesystem::path assets;
        for (int i = 1; i < argc; ++i)
            if (std::string(argv[i]) == "--asset" && i + 1 < argc) {
                asset = true;
                assets = argv[++i];
            }
        PointCloudReconstruction reconstruction(runtime);
        if (asset) {
            SceneDesc scene;
            require(parseSceneFile(assets / "teapot.scn", scene), "Real scene parse");
            const auto frame = loadPointFrame(resolveInputPath(
                assets, scene.pointPath, formatFrameName(scene.pointFile, std::max(0, scene.frame))));
            ReconstructionSettings settings{scene.renderScale, uint32_t(scene.smooth), scene.smoothParams.x,
                                            scene.smoothParams.y};
            reconstruction.reconstruct(frame, settings);
            const auto values = reconstruction.readAtlas();
            uint64_t negative = 0;
            double sum = 0;
            float lo = INFINITY, hi = -INFINITY;
            const auto& packed = reconstruction.readTopology();
            const auto& level = packed.levels[0];
            require(level.nodeExtent.x == 16 && packed.volume.atlasBrickCountAndWidth.w == 18,
                    "Real point-cloud legacy leaf16 configuration");
            const auto res = packed.volume.atlasResolutionAndApron;
            for (uint32_t leaf = 0; leaf < level.nodes.z; ++leaf) {
                const auto* node = packed.nodes.data() + (level.nodes.x + size_t(leaf) * level.nodes.y) / 4;
                for (uint32_t z = 0; z < 16; ++z)
                    for (uint32_t y = 0; y < 16; ++y)
                        for (uint32_t x = 0; x < 16; ++x) {
                            const float v =
                                values[(size_t(node[6] + z) * res.y + node[5] + y) * res.x + node[4] + x];
                            require(std::isfinite(v), "Real reconstructed atlas finite");
                            negative += v < 0;
                            sum += v;
                            lo = std::min(lo, v);
                            hi = std::max(hi, v);
                        }
            }
            require(frame.count() == 1867648 && negative > 0 && lo < 0 && hi > 0,
                    "Real point asset reconstructs a level-set surface");
            require(reconstruction.pointLines().size() == 233456, "Real point overlay cap/stride");
            verifyAssetSamples(reconstruction, frame, settings, values);
            std::cout << "Point-cloud real asset PASS: points=" << frame.count() << " leaves=" << level.nodes.z
                      << " negative=" << negative << " min=" << lo << " max=" << hi << " sum=" << sum << '\n';
            return 0;
        }
        PointFrame positive{
            {.5f, 4, 8, 0},
            {12, 4, 16, 0},
            {0, 0, 0, 32768, 123, 65535, 65535, 65535, 32767}
        };
        ReconstructionSettings settings;
        reconstruction.reconstruct(positive, settings);
        verify(runtime, reconstruction, positive, settings);
        require(reconstruction.generation() == 1, "First generation");
        auto retained = reconstruction.snapshot();
        const auto originalValues = reconstruction.readAtlas();
        PointFrame negative{
            {-25, -17, -9, 0},
            {-13, -5, 3, 0},
            {0, 0, 0, 32768, 32768, 32768, 65535, 65535, 65535, 11000, 22000, 33000, 47000, 4000, 8000}
        };
        settings.smoothingIterations = 2;
        settings.smoothingCenterWeight = 1;
        settings.smoothingBias = -.2f;
        reconstruction.reconstruct(negative, settings);
        verify(runtime, reconstruction, negative, settings);
        gvdb::portable::ChannelSnapshotDesc oldChannel;
        require(NVRHI_SUCCEEDED(retained->getChannel(0, oldChannel)), "Retained previous-frame channel");
        require(oldChannel.raw.byteOffset == 0 && oldChannel.raw.byteSize == originalValues.size() * 4,
                "Retained channel shape");
        std::vector<float> retainedValues(originalValues.size());
        runtime.read(oldChannel.raw.buffer, retainedValues.data(), retainedValues.size() * 4);
        require(retainedValues == originalValues, "Prior snapshot survives owner replacement unchanged");
        require(reconstruction.generation() == 2 && reconstruction.readTopology().volume.boundsMax.x <= 0,
                "Second frame replaces prior positive topology");
        auto invalid = negative;
        invalid.minimum.x = NAN;
        bool rejected = false;
        try {
            reconstruction.reconstruct(invalid, settings);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected && reconstruction.generation() == 2, "Failed frame preserves previous generation");
        verify(runtime, reconstruction, negative, settings);
        const auto beforeFailure = reconstruction.snapshot();
        auto lateFailure = settings;
        lateFailure.smoothingIterations = 4097;
        rejected = false;
        try {
            reconstruction.reconstruct(positive, lateFailure);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected && reconstruction.generation() == 2 && reconstruction.snapshot() == beforeFailure,
                "Late filter rejection preserves exact visible snapshot");
        verify(runtime, reconstruction, negative, settings);
        PointFrame constant{
            {1, 1, 1, 0},
            {1, 1, 1, 0},
            {65535, 32768, 0}
        };
        settings = {};
        reconstruction.reconstruct(constant, settings);
        verify(runtime, reconstruction, constant, settings);
        require(reconstruction.readTopology().levels[0].nodes.z == 1, "Single-brick cloud support");
        std::cout << "Point-cloud GPU reconstruction fixtures PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Point-cloud GPU fixtures: " << e.what() << '\n';
        return 1;
    }
}

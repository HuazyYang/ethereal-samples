// SPDX-License-Identifier: Apache-2.0
#include "../../tests/PortableFixtures.h"
#include "DepthMap.h"
#if ETHEREAL_DEPTH_REFERENCE_TESTS
#include "DepthMapReference.h"
#endif
#include <gvdb/slang/GVDBPortableVbx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
namespace depth_map
{
namespace
{
bool slab(dm::float3 origin, dm::float3 direction, dm::float3 lower, dm::float3 upper, double &first, double &last)
{
    first = 0;
    last = std::numeric_limits<double>::infinity();
    for (unsigned axis = 0; axis < 3; ++axis)
    {
        if (direction[axis] == 0)
        {
            if (origin[axis] < lower[axis] || origin[axis] >= upper[axis])
                return false;
        }
        else
        {
            double a = (lower[axis] - origin[axis]) / double(direction[axis]),
                   b = (upper[axis] - origin[axis]) / double(direction[axis]);
            first = std::max(first, std::min(a, b));
            last = std::min(last, std::max(a, b));
        }
    }
    return first < last;
}
Mesh plane(const View &view, float distance)
{
    Mesh result;
    auto center = view.cameraToWorld.m_translation + view.cameraToWorld.m_linear.row2 * distance;
    auto x = view.cameraToWorld.m_linear.row0 * (distance * 2), y = view.cameraToWorld.m_linear.row1 * (distance * 2);
    result.positions = {center - x - y, center + x - y, center - x + y, center + x + y};
    result.normals.assign(4, -view.cameraToWorld.m_linear.row2);
    result.indices = {0, 1, 2, 2, 1, 3};
    return result;
}
void validate(const Images &image, const View &view, float planeDistance, bool visible)
{
    const auto inverse = dm::inverse(view.indexToWorld);
    const auto origin = inverse.transformPoint(view.cameraToWorld.m_translation);
    const double horizontal = std::tan(dm::radians(25.f)), vertical = horizontal * image.height / image.width;
    const double zExpected =
        visible ? view.farPlane / (view.farPlane - view.nearPlane) -
                      double(view.nearPlane) * view.farPlane / ((view.farPlane - view.nearPlane) * planeDistance)
                : 1;
    size_t checked = 0;
    for (uint32_t y = 0; y < image.height; ++y)
        for (uint32_t x = 0; x < image.width; ++x)
        {
            const size_t pixel = size_t(y) * image.width + x;
            if (!std::isfinite(image.depth[pixel]) || std::abs(image.depth[pixel] - zExpected) > 1e-4)
            {
                std::fprintf(stderr, "raster pixel(%u,%u), plane %f: actual %.9f expected %.9f\n", x, y, planeDistance,
                             image.depth[pixel], zExpected);
                throw std::runtime_error("Raster depth differs from analytic projected plane");
            }
            auto world = view.cameraToWorld.m_linear.row2 +
                         view.cameraToWorld.m_linear.row0 * float(((x + .5) / image.width * 2 - 1) * horizontal) +
                         view.cameraToWorld.m_linear.row1 * float((1 - (y + .5) / image.height * 2) * vertical);
            auto direction = dm::normalize(inverse.transformVector(world));
            auto inWorld = view.indexToWorld.transformVector(direction);
            const double linear = double(view.nearPlane) * view.farPlane /
                                  (view.farPlane - double(image.depth[pixel]) * (view.farPlane - view.nearPlane));
            double limit =
                linear / (view.exactDepth ? dm::dot(inWorld, view.cameraToWorld.m_linear.row2) : dm::length(inWorld));
            if (view.exactDepth)
                limit = std::max(0.0, limit - view.bias);
            unsigned count = 0;
            // Closed-form enumeration of wavefront samples in the two known occupied bricks.
            for (unsigned brick = 0; brick < 2; ++brick)
            {
                double first, last;
                if (!slab(origin, direction, {float(brick * 2), 0, 0}, {float(brick * 2 + 2), 2, 2}, first, last))
                    continue;
                int start = int(std::ceil((first + .001) / view.step));
                for (unsigned i = 0; i < 256; ++i)
                {
                    double t = (start + i) * view.step;
                    if (t >= last || t > limit)
                        break;
                    ++count;
                }
            }
            const unsigned maxCount = unsigned(std::ceil(-std::log(.005) / (.5 * view.step)));
            count = std::min(count, maxCount);
            const double transmission = std::exp(-.5 * view.step * count), alpha = 1 - transmission;
            const double bg[] = {.1, .2, .4}, tint[] = {.8, .4, .2};
            int expected[4];
            for (unsigned c = 0; c < 3; ++c)
                expected[c] = int(std::clamp(bg[c] * transmission + tint[c] * alpha * alpha, 0.0, 1.0) * 255);
            expected[3] = int(alpha * 255);
            const uint32_t packed = image.volume[pixel];
            for (unsigned c = 0; c < 4; ++c)
            {
                int actual = int((packed >> (8 * c)) & 255);
                if (std::abs(actual - expected[c]) > 1)
                {
                    std::fprintf(stderr, "pixel(%u,%u) channel%u: actual%d expected%d samples%u exact%d\n", x, y, c,
                                 actual, expected[c], count, int(view.exactDepth));
                    throw std::runtime_error(
                        "Clipped sparse volume differs from independent constant-density integration");
                }
            }
            const float quantizedAlpha = float(packed >> 24) / 255;
            for (unsigned c = 0; c < 3; ++c)
            {
                const int expectedComposite = int(std::lround(image.polygon[pixel * 4 + c] * (1 - quantizedAlpha) +
                                                              float((packed >> (8 * c)) & 255) * quantizedAlpha));
                if (std::abs(int(image.color[pixel * 4 + c]) - expectedComposite) > 1)
                    throw std::runtime_error("Composition changed reference alpha/quantization behavior");
            }
            ++checked;
        }
    if (checked == 0)
        throw std::runtime_error("Empty analytic image");
}
} // namespace
void runAnalyticTests(nvrhi::IDevice *graphics, nvrhi::IDevice *compute)
{
    namespace g = gvdb::slang;
    auto fixture = g::fixtures::twoBrickVolume();
    Scene scene;
    std::string error;
    g::VbxRawVolume source;
    if (!g::packVolume(g::fixtures::volumeSource(fixture), source.packed, error))
        throw std::runtime_error(error);
    g::VbxChannel scalar;
    scalar.format = g::AtlasFormat::R32_FLOAT;
    scalar.bytes.resize(fixture.atlas.size() * sizeof(float));
    std::memcpy(scalar.bytes.data(), fixture.atlas.data(), scalar.bytes.size());
    source.channels.push_back(std::move(scalar));
    if (!g::saveRawVBX(source, scene.volumeVBX, error))
        throw std::runtime_error(error);
    scene.refreshApron = false;
    scene.transfer.assign(16384, {.8f, .4f, .2f, .5f});
    Renderer renderer(graphics, compute, scene);
    const auto reject = [&](Mesh mesh) {
        bool rejected = false;
        try
        {
            renderer.setMesh(mesh);
        }
        catch (const std::runtime_error &)
        {
            rejected = true;
        }
        if (!rejected)
            throw std::runtime_error("Invalid mesh reached GPU upload");
    };
    Mesh invalid = plane(defaultView(), 1);
    invalid.indices[0] = uint32_t(invalid.positions.size());
    reject(invalid);
    invalid = plane(defaultView(), 1);
    invalid.positions[0].x = std::numeric_limits<float>::quiet_NaN();
    reject(invalid);
    unsigned cases = 0;
    for (unsigned transform = 0; transform < 2; ++transform)
    {
        View view;
        if (transform)
            view.indexToWorld = dm::scaling(dm::float3(.5f, 1.25f, 2.f)) *
                                dm::rotationQuat(dm::radians(dm::float3(17, 33, -9))).toAffine() *
                                dm::translation(dm::float3(5, -3, 7));
        const auto &m = view.indexToWorld;
        view.cameraToWorld = {dm::float3x3(dm::normalize(m.transformVector({0, 0, 1})),
                                           dm::normalize(m.transformVector({0, 1, 0})),
                                           dm::normalize(m.transformVector({1, 0, 0}))),
                              m.transformPoint({-1, 1, 1})};
        view.light = view.cameraToWorld.m_translation;
        for (unsigned exact = 0; exact < 2; ++exact)
        {
            view.exactDepth = exact != 0;
            view.bias = exact ? .13f : 1.f; // Legacy bias must remain inert.
            for (float distance : {.05f, .3f, 1.183f, 7.f, 1100.f})
            {
                renderer.setMesh(plane(view, distance));
                // Repeat without readback/host waits, then resize and recreate both imported images.
                const uint32_t w = cases % 2 ? 31 : 65, h = cases % 2 ? 47 : 33;
                for (unsigned frame = 0; frame < 3; ++frame)
                    renderer.render(w, h, view);
                auto result = renderer.read();
                validate(result, view, distance, true);
#if ETHEREAL_DEPTH_REFERENCE_TESTS
                compareTypedReference(compute, scene, result, view);
#endif
                ++cases;
            }
        }
    }
    View empty;
    empty.cameraToWorld = {dm::float3x3({0, 0, 1}, {0, 1, 0}, {1, 0, 0}), {-1, 1, 1}};
    empty.light = empty.cameraToWorld.m_translation;
    renderer.setMesh({});
    renderer.render(19, 11, empty);
    auto emptyImage = renderer.read();
    validate(emptyImage, empty, 0, false);
#if ETHEREAL_DEPTH_REFERENCE_TESTS
    compareTypedReference(compute, scene, emptyImage, empty);
    // Poison only aprons. Keeping imported values and rebuilding sparse borders
    // must both agree with the old path, and must yield different images.
    for (uint32_t z = 0; z < 4; ++z)
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 8; ++x)
            {
                const float value = (x % 4 >= 1 && x % 4 <= 2 && y >= 1 && y <= 2 && z >= 1 && z <= 2) ? .35f : 42.f;
                std::memcpy(source.channels[0].bytes.data() + ((z * 4 + y) * 8 + x) * 4, &value, 4);
            }
    if (!g::saveRawVBX(source, scene.volumeVBX, error))
        throw std::runtime_error(error);
    scene.transfer = referenceTransfer();
    std::vector<uint32_t> importedImage;
    for (bool refresh : {false, true})
    {
        scene.refreshApron = refresh;
        Renderer candidate(graphics, compute, scene);
        candidate.render(65, 33, empty);
        const auto image = candidate.read();
        compareTypedReference(compute, scene, image, empty);
        if (!refresh)
            importedImage = image.volume;
        else if (importedImage == image.volume)
            throw std::runtime_error("Apron refresh did not change poisoned sparse boundary");
    }
    std::printf("PASS exact typed/raw comparison, imported apron preservation and sparse apron refresh\n");
#endif
    std::printf("PASS %u depth-map analytic cases: projected plane, depth clipping, off-axis rays, transformed "
                "instances, reference alpha, repeated GPU ownership and resize\n",
                cases + 1);
}
} // namespace depth_map

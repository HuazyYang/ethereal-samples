// SPDX-License-Identifier: Apache-2.0
#include "../gvdb_samples/gvdb/GVDBSlangRayTracing.h"
#include "../gvdb_samples/tests/PortableFixtures.h"
#include "ComputeRuntime.h"
#include <nvrhi/core/foundation.h>
#ifndef ETHEREAL_RT_STANDALONE
#define ETHEREAL_RT_STANDALONE 0
#endif
#if !ETHEREAL_RT_STANDALONE
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#endif
#if ETHEREAL_TEST_CUDA
#include <nvrhi/cuda.h>
#endif
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
using namespace nvrhi;
namespace g = gvdb::slang;
namespace f = gvdb::slang::fixtures;
using ethereal::slang::ComputeRuntime;
namespace
{
struct Messages : ObjectImpl<IMessageCallback>
{
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity, const char *text) noexcept override
    {
        std::fprintf(stderr, "NVRHI: %s\n", text);
    }
};
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
void expectNear(float actual, float expected, const char *message, float epsilon = 1e-4f)
{
    if (std::abs(actual - expected) > epsilon || !std::isfinite(actual))
    {
        std::fprintf(stderr, "%s: expected %.8f actual %.8f\n", message, expected, actual);
        throw std::runtime_error(message);
    }
}
struct Pixel
{
    g::Float4 color;
    float depth;
};
Pixel read(ComputeRuntime &runtime, const g::RaycastOutput &output)
{
    Pixel pixel{};
    runtime.read(output.color, &pixel.color, sizeof(pixel.color));
    runtime.read(output.depth, &pixel.depth, sizeof(pixel.depth));
    return pixel;
}
void color(const g::Float4 &actual, const g::Float4 &expected)
{
    expectNear(actual.x, expected.x, "red");
    expectNear(actual.y, expected.y, "green");
    expectNear(actual.z, expected.z, "blue");
    expectNear(actual.w, expected.w, "alpha");
}
int run(IDevice *device, const std::filesystem::path &library)
{
    if (!device->queryFeatureSupport(Feature::RayTracingPipeline))
    {
        std::puts("SKIP ray tracing unavailable");
        return 77;
    }
    ComputeRuntime runtime(device, library.parent_path());
    auto fixture = f::twoBrickVolume();
    g::PackedVolume packed;
    std::string error;
    require(g::packVolume(f::volumeSource(fixture), packed, error), error.c_str());
    g::SlangVolume volume(runtime, packed, fixture.atlas.data(), fixture.atlas.size());
    g::SlangVolumeRT renderer(runtime, volume, library);
    const std::vector<g::Float4> transfer = {{1, .5f, .25f, .5f}, {1, .5f, .25f, .5f}};
    auto p = f::raycastParameters(fixture);
    auto pixel = read(runtime, renderer.render(p, transfer));
    color(pixel.color, f::raycastExpected());
    expectNear(pixel.depth, 1, "volume depth");
    p.march.y = 2;
    p.march.z = 4;
    pixel = read(runtime, renderer.render(p, transfer));
    const float clipped = 1 - std::exp(-1.f);
    color(pixel.color, {clipped, .5f * clipped, .25f * clipped, clipped});
    expectNear(pixel.depth, 2, "clipped volume depth");
    std::puts("PASS GVDB procedural RT sparse volume, analytic Beer-Lambert color and clipped ray interval");
    p = f::raycastParameters(fixture);
    p.rayOrigin = {9, 3, .5f, 0};
    rt::AffineTransform transform = {2, 0, 0, 10, 0, 3, 0, 0, 0, 0, .5f, 0};
    pixel = read(runtime, renderer.render(p, transfer, false, transform));
    color(pixel.color, f::raycastExpected());
    expectNear(pixel.depth, 1, "transformed volume depth");
    transform[3] = 30;
    p.march.z = 10;
    p.background = {.1f, .2f, .3f, .4f};
    pixel = read(runtime, renderer.render(p, transfer, false, transform));
    color(pixel.color, p.background);
    require(std::isinf(pixel.depth), "miss depth infinity");
    std::puts("PASS GVDB RT nonuniform instance transform, TLAS update and miss background");
    // A linear field sampleVolume(x)=x/4 has an isosurface at x=2. Include each
    // brick's apron to keep the interpolation analytic across the brick boundary.
    for (uint32_t z = 0; z < 4; ++z)
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 8; ++x)
            {
                const float world = float(x % 4) - .5f + float(x / 4) * 2.f;
                fixture.atlas[(z * 4 + y) * 8 + x] = world * .25f;
            }
    runtime.upload(volume.atlas(), fixture.atlas.data(), fixture.atlas.size() * sizeof(float));
    p = f::raycastParameters(fixture);
    pixel = read(runtime, renderer.render(p, transfer, true));
    color(pixel.color, {1, .5f, .25f, 1});
    expectNear(pixel.depth, 3, "surface depth", .002f);
    std::puts("PASS GVDB procedural isosurface, gradient normal, shadow-ray recursion and shared atlas update");
    bool rejected = false;
    rt::AffineTransform singular{};
    try
    {
        renderer.render(p, transfer, false, singular);
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    require(rejected, "singular instance transform rejected");
    return 0;
}
} // namespace
int main(int argc, const char **argv)
{
#if !ETHEREAL_RT_STANDALONE
    donut::log::ConsoleApplicationMode();
#endif
    try
    {
        std::filesystem::path library;
        for (int i = 1; i + 1 < argc; ++i)
            if (std::strcmp(argv[i], "--library") == 0)
                library = argv[i + 1];
#if ETHEREAL_TEST_CUDA
        bool cudaRequested = ETHEREAL_RT_STANDALONE != 0;
        for (int i = 1; i < argc; ++i)
            if (std::strcmp(argv[i], "-cuda") == 0)
                cudaRequested = true;
        if (cudaRequested)
        {
            auto callback = MAKE_RC_OBJ_PTR(Messages);
            cuda::DeviceDesc desc;
            desc.errorCB = callback;
            desc.enableOptiX = true;
            auto device = cuda::createDevice(desc);
            if (!device)
            {
                std::puts("SKIP CUDA device unavailable");
                return 77;
            }
#if !ETHEREAL_RT_STANDALONE
            if (library.empty())
                library =
                    donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_slang" / "optix" / "GVDBRayTracing.bin";
#endif
            require(!library.empty(), "--library must name an OptiX artifact");
            return run(device, library);
        }
#endif
#if !ETHEREAL_RT_STANDALONE
        const auto api = donut::app::GetGraphicsAPIFromCommandLine(argc, argv);
        auto manager = TakeOver(donut::app::DeviceManager::Create(api));
        require(bool(manager), "Backend unavailable");
        donut::app::DeviceCreationParameters params;
        params.enableNvrhiValidationLayer = true;
        params.enableRayTracingExtensions = true;
        require(manager->CreateHeadlessDevice(params), "Device creation failed");
        if (library.empty())
            library = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_slang" /
                      donut::app::GetShaderTypeName(api) / "GVDBRayTracing.bin";
        const int result = run(manager->GetDevice(), library);
        manager->Shutdown();
        return result;
#else
        return 77;
#endif
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}

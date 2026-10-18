// Headless GPU checks for the Slang sample render paths.
#pragma once

#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace slang_render_test
{
    enum class Image { Triangle, RayTracedTriangle, Specializations };

    inline bool Requested(int argc, const char* const* argv)
    {
        for (int i = 1; i < argc; ++i)
            if (std::strcmp(argv[i], "--test-headless") == 0)
                return true;
        return false;
    }

    inline bool CheckImage(const uint8_t* data, size_t pitch, uint32_t width, uint32_t height, Image expected)
    {
        const auto pixel = [&](uint32_t x, uint32_t y) { return data + y * pitch + x * 4; };
        const auto closeTo = [](int value, int reference) { return value >= reference - 3 && value <= reference + 3; };
        const auto corner = pixel(4, 4);
        const int background = expected == Image::RayTracedTriangle ? 51 : 0;
        if (!closeTo(corner[0], background) || !closeTo(corner[1], background) || !closeTo(corner[2], background))
        {
            std::fprintf(stderr, "Unexpected background RGB: %u %u %u\n", corner[0], corner[1], corner[2]);
            return false;
        }
        if (expected == Image::Specializations)
        {
            const uint8_t colors[4][3] = { {255,0,0}, {0,255,0}, {0,0,255}, {255,0,255} };
            for (uint32_t i = 0; i < 4; ++i)
            {
                const auto p = pixel(width * (2 * i + 1) / 8, height / 2);
                if (!closeTo(p[0], colors[i][0]) || !closeTo(p[1], colors[i][1]) || !closeTo(p[2], colors[i][2]))
                {
                    std::fprintf(stderr, "Specialization %u RGB: %u %u %u\n", i, p[0], p[1], p[2]);
                    return false;
                }
            }
        }
        else
        {
            const auto center = pixel(width / 2, height / 2);
            // Both triangle paths interpolate three vertex colors at the center.
            if (center[0] < 40 || center[1] < 40 || center[2] < 40 ||
                !closeTo(int(center[0]) + center[1] + center[2], 255))
            {
                std::fprintf(stderr, "Unexpected triangle center RGB: %u %u %u\n", center[0], center[1], center[2]);
                return false;
            }
        }
        // Coverage detects a stale clear, missing geometry, or incorrectly sized dispatch.
        uint32_t covered = 0;
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const auto p = pixel(x, y);
                if (!closeTo(p[0], background) || !closeTo(p[1], background) || !closeTo(p[2], background))
                    ++covered;
            }
        const double fraction = double(covered) / double(width * height);
        const double expectedFraction = expected == Image::Triangle ? 0.125 : expected == Image::Specializations ? 0.25 : 0.5;
        if (fraction < expectedFraction - 0.02 || fraction > expectedFraction + 0.02)
        {
            std::fprintf(stderr, "Triangle coverage %.4f, expected %.4f\n", fraction, expectedFraction);
            return false;
        }
        std::printf("Slang render readback passed; coverage %.4f\n", fraction);
        return true;
    }

    template<class Sample>
    int Run(donut::app::DeviceManager* manager, donut::app::DeviceCreationParameters params, Image expected)
    {
        donut::log::ConsoleApplicationMode();
        std::atomic<unsigned> errors{0};
        const auto oldCallback = donut::log::GetCallback();
        donut::log::SetCallback([&](donut::log::Severity severity, const char* message) {
            if (severity >= donut::log::Severity::Error) ++errors;
            std::fprintf(stderr, "%s\n", message);
        });
        // Enable API-independent validation even in release builds. Native debug runtimes
        // remain opt-in so a missing system debug layer cannot hide rendering coverage.
        params.enableNvrhiValidationLayer = true;
        const int result = [&]() -> int {
            if (!manager || !manager->CreateHeadlessDevice(params)) return 1;
            auto device = manager->GetDevice();
            if (expected == Image::RayTracedTriangle && !device->queryFeatureSupport(nvrhi::Feature::RayTracingPipeline))
            {
                std::fprintf(stderr, "SKIP: adapter does not support ray tracing pipelines\n");
                return 77;
            }
            Sample example(manager);
            if (!example.Init()) return 1;
            nvrhi::TextureDesc desc;
            desc.width = 256;
            desc.height = 128;
            desc.format = nvrhi::Format::RGBA8_UNORM;
            desc.isRenderTarget = true;
            desc.initialState = nvrhi::ResourceStates::RenderTarget;
            desc.keepInitialState = true;
            desc.debugName = "SlangRenderReadback";
            nvrhi::TextureHandle texture;
            device->createTexture(desc, &texture);
            if (!texture) return 1;
            nvrhi::FramebufferHandle framebuffer;
            device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture), &framebuffer);
            if (!framebuffer) return 1;
            example.Render(framebuffer);
            nvrhi::StagingTextureHandle staging;
            device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read, &staging);
            nvrhi::CommandListHandle commands;
            device->createCommandList(nvrhi::CommandListParameters(), &commands);
            if (!staging || !commands) return 1;
            commands->open();
            commands->copyTexture2(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice());
            commands->close();
            device->executeCommandList(commands);
            device->waitForIdle();
            size_t pitch = 0;
            const auto data = static_cast<const uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, pitch));
            if (!data) return 1;
            const bool passed = pitch >= desc.width * 4 && CheckImage(data, pitch, desc.width, desc.height, expected);
            device->unmapStagingTexture(staging);
            return passed ? 0 : 1;
        }();
        if (manager) manager->Shutdown();
        donut::log::SetCallback(oldCallback);
        if (errors != 0) return 1;
        return result;
    }
}

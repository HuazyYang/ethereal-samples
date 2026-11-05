/*
* Copyright (c) 2026, ethereal contributors. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

// Deterministic runs of a sample, for comparing a sample with its render-graph version (aggregate plan
// docs/plans/2026-11-05-render-graph.md, step P2.0):
//
//     -frames N         exit after N frames
//     -screenshot F     save the back buffer of the last frame to F (.png, .bmp, .tga, .jpg)
//     -fixedstep        Animate sees 1/60 s per frame instead of the measured time
//     -timings F        write the median CPU and GPU frame times after the warm-up to F (JSON)
//     -warmup N         frames before the timings (default 100)
//
// A Harness adds a layer in front of the sample's (the frame's start: CPU clock and a GPU timer query) and one
// behind it (the frame's end, the screenshot, the exit). The sample calls sample_harness::Step from Animate.

#pragma once

#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <nvrhi/utils.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace sample_harness
{
    struct Options
    {
        uint32_t frames = 0;        // 0: run until the window closes
        uint32_t warmup = 100;
        bool fixedStep = false;
        std::string screenshot;
        std::string timings;
    };

    inline Options ParseOptions(int argc, const char* const* argv)
    {
        Options options;
        for (int i = 1; i < argc; ++i)
        {
            const char* arg = argv[i];
            const bool hasValue = i + 1 < argc;
            if (!std::strcmp(arg, "-frames") && hasValue)
                options.frames = uint32_t(std::strtoul(argv[++i], nullptr, 10));
            else if (!std::strcmp(arg, "-warmup") && hasValue)
                options.warmup = uint32_t(std::strtoul(argv[++i], nullptr, 10));
            else if (!std::strcmp(arg, "-screenshot") && hasValue)
                options.screenshot = argv[++i];
            else if (!std::strcmp(arg, "-timings") && hasValue)
                options.timings = argv[++i];
            else if (!std::strcmp(arg, "-fixedstep"))
                options.fixedStep = true;
        }
        return options;
    }

    class Harness;

    namespace detail
    {
        inline Harness*& Current()
        {
            static Harness* harness = nullptr;
            return harness;
        }
    }

    class Harness
    {
    public:
        Harness(donut::app::DeviceManager* deviceManager, const Options& options)
            : m_DeviceManager(deviceManager)
            , m_Options(options)
            , m_Begin(deviceManager, this, true)
            , m_End(deviceManager, this, false)
        {
            detail::Current() = this;
        }

        ~Harness()
        {
            Detach();
            if (detail::Current() == this)
                detail::Current() = nullptr;
        }

        Harness(const Harness&) = delete;
        Harness& operator=(const Harness&) = delete;

        const Options& GetOptions() const { return m_Options; }

        // After the sample's layer was added: the harness's layers go around it.
        void Attach()
        {
            if (m_Attached)
                return;
            m_Attached = true;
            m_DeviceManager->AddLayerToFront(&m_Begin);
            m_DeviceManager->AddLayerToBack(&m_End);
        }

        // After the message loop: writes the timings.
        void Detach()
        {
            if (!m_Attached)
                return;
            m_Attached = false;
            m_DeviceManager->GetDevice()->waitForIdle();
            CollectTimers(true);
            m_DeviceManager->RemoveLayer(&m_Begin);
            m_DeviceManager->RemoveLayer(&m_End);
            WriteTimings();
            m_Queries = {};
            m_CommandList = nullptr;
            m_CommonPasses = nullptr;
            m_ShaderFactory = nullptr;
        }

        float Step(float seconds) const { return m_Options.fixedStep ? 1.0f / 60.0f : seconds; }

    private:
        class Layer : public donut::app::IAppLayer
        {
            NVRHI_INHERIT_INTERFACE_TABLE()
        public:
            Layer(donut::app::DeviceManager* deviceManager, Harness* harness, bool begin)
                : IAppLayer(deviceManager), m_Harness(harness), m_IsBegin(begin)
            { }
            bool ShouldRenderUnfocused() override { return true; }
            bool ShouldAnimateUnfocused() override { return true; }
            bool SupportsDepthBuffer() override { return false; }
            void Render(nvrhi::IFramebuffer*) override
            {
                if (m_IsBegin)
                    m_Harness->BeginFrame();
                else
                    m_Harness->EndFrame();
            }
        private:
            Harness* m_Harness;
            bool m_IsBegin;
        };

        using Clock = std::chrono::steady_clock;
        static constexpr uint32_t c_Queries = 8;

        void BeginFrame()
        {
            const Clock::time_point now = Clock::now();
            if (m_Frame > m_Options.warmup && m_Frame <= TimedEnd())
                m_CpuMs.push_back(std::chrono::duration<double, std::milli>(now - m_FrameStart).count());
            m_FrameStart = now;

            nvrhi::IDevice* device = m_DeviceManager->GetDevice();
            if (!m_CommandList)
                device->createCommandList(nvrhi::CommandListParameters(), m_CommandList.ReleaseAndGetAddressOf());
            CollectTimers(false);
            nvrhi::AutoPtr<nvrhi::ITimerQuery>& query = m_Queries[m_Frame % c_Queries];
            if (!query)
                device->createTimerQuery(query.ReleaseAndGetAddressOf());
            m_QueryFrame[m_Frame % c_Queries] = m_Frame;
            m_QueryPending[m_Frame % c_Queries] = true;
            m_CommandList->open();
            m_CommandList->beginTimerQuery(query);
            m_CommandList->close();
            device->executeCommandList(m_CommandList);
        }

        void EndFrame()
        {
            nvrhi::IDevice* device = m_DeviceManager->GetDevice();
            m_CommandList->open();
            m_CommandList->endTimerQuery(m_Queries[m_Frame % c_Queries]);
            m_CommandList->close();
            device->executeCommandList(m_CommandList);

            ++m_Frame;
            if (!m_Options.frames || m_Frame < m_Options.frames)
                return;
            if (!m_Options.screenshot.empty())
            {
                device->waitForIdle();
                // A back buffer in another format than RGBA8 (Vulkan's is BGRA8) is blitted to RGBA8 first.
                nvrhi::ITexture* backBuffer = m_DeviceManager->GetCurrentBackBuffer();
                const nvrhi::Format format = backBuffer->getDesc().format;
                if (format != nvrhi::Format::RGBA8_UNORM && format != nvrhi::Format::SRGBA8_UNORM && !m_CommonPasses)
                {
                    const std::filesystem::path shaders = donut::app::GetDirectoryWithExecutable() / "shaders/framework" /
                        donut::app::GetShaderTypeName(device->getGraphicsAPI());
                    nvrhi::AutoPtr<donut::vfs::RootFileSystem> rootFS = MAKE_RC_OBJ_PTR(donut::vfs::RootFileSystem);
                    rootFS->mount("/shaders/donut", shaders);
                    m_ShaderFactory = MAKE_RC_OBJ_PTR(donut::engine::ShaderFactory, device, rootFS, "/shaders");
                    m_CommonPasses = MAKE_RC_OBJ_PTR(donut::engine::CommonRenderPasses, device, m_ShaderFactory);
                }
                if (donut::engine::SaveTextureToFile(device, m_CommonPasses, backBuffer, nvrhi::ResourceStates::Present,
                                                     m_Options.screenshot.c_str(), false))
                    donut::log::info("Saved the frame %u back buffer to %s", m_Frame, m_Options.screenshot.c_str());
                else
                    donut::log::error("Could not save the back buffer to %s", m_Options.screenshot.c_str());
            }
            glfwSetWindowShouldClose(m_DeviceManager->GetWindow(), GLFW_TRUE);
        }

        uint32_t TimedEnd() const { return m_Options.frames ? m_Options.frames - 1 : UINT32_MAX; }

        void CollectTimers(bool wait)
        {
            nvrhi::IDevice* device = m_DeviceManager->GetDevice();
            for (uint32_t i = 0; i < c_Queries; ++i)
            {
                if (!m_QueryPending[i] || !m_Queries[i])
                    continue;
                if (!wait && !device->pollTimerQuery(m_Queries[i]))
                    continue;
                const float seconds = device->getTimerQueryTime(m_Queries[i]);
                device->resetTimerQuery(m_Queries[i]);
                m_QueryPending[i] = false;
                if (m_QueryFrame[i] >= m_Options.warmup && m_QueryFrame[i] < TimedEnd())
                    m_GpuMs.push_back(double(seconds) * 1000.0);
            }
        }

        static double Median(std::vector<double> samples)
        {
            if (samples.empty())
                return 0.0;
            std::sort(samples.begin(), samples.end());
            return samples[samples.size() / 2];
        }

        void WriteTimings()
        {
            if (m_Options.timings.empty())
                return;
            FILE* file = std::fopen(m_Options.timings.c_str(), "w");
            if (!file)
            {
                donut::log::error("Could not write %s", m_Options.timings.c_str());
                return;
            }
            const char* api = nvrhi::utils::GraphicsAPIToString(m_DeviceManager->GetDevice()->getGraphicsAPI());
            std::fprintf(file, "{\n  \"api\": \"%s\",\n  \"frames\": %u,\n  \"warmup\": %u,\n  \"cpu_frames\": %u,\n"
                "  \"gpu_frames\": %u,\n  \"median_cpu_frame_ms\": %.4f,\n  \"median_gpu_frame_ms\": %.4f\n}\n",
                api, m_Frame, m_Options.warmup, uint32_t(m_CpuMs.size()), uint32_t(m_GpuMs.size()), Median(m_CpuMs), Median(m_GpuMs));
            std::fclose(file);
        }

        donut::app::DeviceManager* m_DeviceManager;
        Options m_Options;
        Layer m_Begin;
        Layer m_End;
        bool m_Attached = false;
        uint32_t m_Frame = 0;
        Clock::time_point m_FrameStart;
        nvrhi::AutoPtr<nvrhi::ICommandList> m_CommandList;
        nvrhi::AutoPtr<donut::engine::ShaderFactory> m_ShaderFactory;
        nvrhi::AutoPtr<donut::engine::CommonRenderPasses> m_CommonPasses;
        std::array<nvrhi::AutoPtr<nvrhi::ITimerQuery>, c_Queries> m_Queries;
        std::array<uint32_t, c_Queries> m_QueryFrame = {};
        std::array<bool, c_Queries> m_QueryPending = {};
        std::vector<double> m_CpuMs;
        std::vector<double> m_GpuMs;
    };

    // Animate's time step: fixed with -fixedstep.
    inline float Step(float seconds)
    {
        const Harness* harness = detail::Current();
        return harness ? harness->Step(seconds) : seconds;
    }
}

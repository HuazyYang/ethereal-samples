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

// Parallel recording for the render-graph samples (aggregate plan docs/plans/2026-11-05-render-graph.md, step P2.4):
//     -parallel N      records the graph's segments on N threads, the caller and N - 1 workers, through
//                      ExecuteDesc::parallelFor (design 10.1). D3D11 records on the calling thread whatever N is.
//     -granularity G   CompileOptions::recordGranularity: a segment also ends after G passes (0: only at queue changes
//                      and cross-queue waits). With -parallel it defaults to 3: each segment opens, closes and sets up
//                      a list, and in async_compute_rdg three passes per segment recorded fastest on D3D12 and Vulkan
//                      together (P2.4).

#pragma once

#include <donut/app/RenderGraphLayer.h>
#include <donut/render/RenderGraphWorkerPool.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace sample_harness
{
    class GraphRecording
    {
    public:
        void Configure(int argc, const char* const* argv, donut::app::RenderGraphLayer* layer)
        {
            uint32_t threads = 1;
            int granularity = -1;
            for (int i = 1; i + 1 < argc; ++i)
            {
                if (!std::strcmp(argv[i], "-parallel"))
                    threads = uint32_t(std::max(1, std::atoi(argv[i + 1])));
                else if (!std::strcmp(argv[i], "-granularity"))
                    granularity = std::max(0, std::atoi(argv[i + 1]));
            }
            if (threads > 1)
            {
                m_Pool = std::make_unique<donut::render::RenderGraphWorkerPool>(threads - 1);
                donut::render::ExecuteDesc& execute = layer->GetExecuteDesc();
                execute.parallelFor = &donut::render::RenderGraphWorkerPool::ParallelFor;
                execute.schedulerContext = m_Pool.get();
                execute.workerCount = m_Pool->ThreadCount();
                if (granularity < 0)
                    granularity = 3;
            }
            if (granularity > 0)
                layer->GetCompileOptions().recordGranularity = uint32_t(granularity);
        }

        [[nodiscard]] uint32_t GetThreadCount() const { return m_Pool ? m_Pool->ThreadCount() : 1; }

    private:
        std::unique_ptr<donut::render::RenderGraphWorkerPool> m_Pool;
    };
}

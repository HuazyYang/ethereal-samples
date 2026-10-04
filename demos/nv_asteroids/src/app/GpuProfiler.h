#pragma once

#include <nvrhi/nvrhi.h>

#include <string>
#include <unordered_map>
#include <vector>

// Developer aid (deviation, not in the binary): per-pass GPU timing, enabled by "-gpuProfile" / "-perfLog".
// The passes bracket their work with ProfBegin/ProfEnd instead of beginMarker/endMarker (the binary called the
// nvrhi markers directly). With the profiler disabled the two helpers are exactly the marker calls.
namespace demo
{
    class GpuProfiler
    {
    public:
        static GpuProfiler& Get();

        void Enable(nvrhi::IDevice* device);
        bool IsEnabled() const { return m_Device != nullptr; }

        // Collects the timers of the frame that used the next slot (kSlots frames ago) and starts a new frame.
        void BeginFrame();
        void Begin(nvrhi::ICommandList* commandList, const char* name);
        void End(nvrhi::ICommandList* commandList);

        // Per-scope GPU milliseconds averaged over the frames collected since the last call, one line per scope
        // (nesting shown by indentation). Resets the accumulators.
        std::string TakeReport();

    private:
        static constexpr size_t kSlots = 4;

        struct Scope
        {
            std::string path;
            std::string name;
            int depth = 0;
            nvrhi::TimerQueryHandle query;
        };
        struct Slot
        {
            std::vector<Scope> scopes;
            size_t used = 0;
        };
        struct Accum
        {
            std::string name;
            int depth = 0;
            double totalMs = 0.0;
        };

        nvrhi::DeviceHandle m_Device;
        Slot m_Slots[kSlots];
        size_t m_Current = 0;
        std::vector<size_t> m_Stack;
        std::vector<Accum> m_Accums;
        std::unordered_map<std::string, size_t> m_AccumIndex;
        uint32_t m_FramesCollected = 0;
    };

    inline void ProfBegin(nvrhi::ICommandList* commandList, const char* name)
    {
        GpuProfiler& profiler = GpuProfiler::Get();
        if (profiler.IsEnabled())
            profiler.Begin(commandList, name);
        else
            commandList->beginMarker(name);
    }

    inline void ProfEnd(nvrhi::ICommandList* commandList)
    {
        GpuProfiler& profiler = GpuProfiler::Get();
        if (profiler.IsEnabled())
            profiler.End(commandList);
        else
            commandList->endMarker();
    }
}

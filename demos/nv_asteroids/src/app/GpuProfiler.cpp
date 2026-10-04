#include "app/GpuProfiler.h"

#include <cstdio>

namespace demo
{
    GpuProfiler& GpuProfiler::Get()
    {
        static GpuProfiler instance;
        return instance;
    }

    void GpuProfiler::Enable(nvrhi::IDevice* device)
    {
        m_Device = device;
    }

    void GpuProfiler::BeginFrame()
    {
        if (!m_Device)
            return;

        m_Current = (m_Current + 1) % kSlots;
        Slot& slot = m_Slots[m_Current];

        bool collected = false;
        for (size_t i = 0; i < slot.used; ++i)
        {
            Scope& scope = slot.scopes[i];
            if (m_Device->pollTimerQuery(scope.query))
            {
                const double ms = double(m_Device->getTimerQueryTime(scope.query)) * 1000.0;
                auto it = m_AccumIndex.find(scope.path);
                if (it == m_AccumIndex.end())
                {
                    it = m_AccumIndex.emplace(scope.path, m_Accums.size()).first;
                    m_Accums.push_back({ scope.name, scope.depth, 0.0 });
                }
                m_Accums[it->second].totalMs += ms;
                collected = true;
            }
            m_Device->resetTimerQuery(scope.query);
        }
        if (collected)
            ++m_FramesCollected;

        slot.used = 0;
        m_Stack.clear();
    }

    void GpuProfiler::Begin(nvrhi::ICommandList* commandList, const char* name)
    {
        Slot& slot = m_Slots[m_Current];
        if (slot.used == slot.scopes.size())
        {
            slot.scopes.emplace_back();
            slot.scopes.back().query = m_Device->createTimerQuery();
        }

        Scope& scope = slot.scopes[slot.used];
        scope.name = name;
        scope.depth = int(m_Stack.size());
        scope.path = m_Stack.empty() ? std::string(name) : slot.scopes[m_Stack.back()].path + "/" + name;
        m_Stack.push_back(slot.used);
        ++slot.used;

        commandList->beginMarker(name);
        commandList->beginTimerQuery(scope.query);
    }

    void GpuProfiler::End(nvrhi::ICommandList* commandList)
    {
        if (m_Stack.empty())
        {
            commandList->endMarker();
            return;
        }

        Scope& scope = m_Slots[m_Current].scopes[m_Stack.back()];
        m_Stack.pop_back();
        commandList->endTimerQuery(scope.query);
        commandList->endMarker();
    }

    std::string GpuProfiler::TakeReport()
    {
        std::string text;
        if (m_FramesCollected == 0)
            return text;

        char line[160];
        for (const Accum& accum : m_Accums)
        {
            snprintf(line, sizeof(line), "  gpu %*s%-*s %7.3f ms\n", accum.depth * 2, "", 32 - accum.depth * 2, accum.name.c_str(),
                accum.totalMs / double(m_FramesCollected));
            text += line;
        }

        for (Accum& accum : m_Accums)
            accum.totalMs = 0.0;
        m_FramesCollected = 0;
        return text;
    }
}

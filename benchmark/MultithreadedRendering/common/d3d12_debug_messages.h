#pragma once
// -debug: reads the D3D12 debug layer's messages through ID3D12InfoQueue and prints them to stderr,
// one line per distinct message id, instead of letting them go to OutputDebugString (and, for the
// nvrhi executable, instead of Donut's break-on-error, which has no debugger to break into here).
// Used by both renderers so that a validation run leaves its findings in the console log.

#include <d3d12.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace Benchmark
{

class D3D12DebugMessages
{
public:
    // Returns false when the debug layer is not active (no info queue on the device).
    bool Init(ID3D12Device* device)
    {
        if (!device || FAILED(device->QueryInterface(__uuidof(ID3D12InfoQueue), reinterpret_cast<void**>(&m_InfoQueue))))
            return false;
        m_InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        m_InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        m_InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
        m_InfoQueue->SetMuteDebugOutput(TRUE);
        m_InfoQueue->SetMessageCountLimit(4096);
        return true;
    }

    ~D3D12DebugMessages()
    {
        Drain();
        if (m_InfoQueue) m_InfoQueue->Release();
    }

    bool Active() const { return m_InfoQueue != nullptr; }
    unsigned Errors() const { return m_Errors; }
    unsigned Warnings() const { return m_Warnings; }

    // Call once per frame (and at shutdown).
    void Drain()
    {
        if (!m_InfoQueue) return;
        const UINT64 count = m_InfoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i)
        {
            SIZE_T length = 0;
            if (FAILED(m_InfoQueue->GetMessage(i, nullptr, &length)) || length == 0) continue;
            m_Storage.resize(length);
            D3D12_MESSAGE* message = reinterpret_cast<D3D12_MESSAGE*>(m_Storage.data());
            if (FAILED(m_InfoQueue->GetMessage(i, message, &length))) continue;
            if (message->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue; // info / message level is noise
            if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) ++m_Warnings; else ++m_Errors;
            if (std::find(m_Reported.begin(), m_Reported.end(), int(message->ID)) != m_Reported.end()) continue;
            m_Reported.push_back(int(message->ID));
            fprintf(stderr, "[d3d12 %s %d] %s\n", message->Severity == D3D12_MESSAGE_SEVERITY_WARNING ? "warning" : "error", int(message->ID),
                    message->pDescription);
        }
        if (count) fflush(stderr);
        m_InfoQueue->ClearStoredMessages();
    }

private:
    ID3D12InfoQueue* m_InfoQueue = nullptr;
    std::vector<char> m_Storage;
    std::vector<int> m_Reported;
    unsigned m_Errors = 0, m_Warnings = 0;
};

} // namespace Benchmark

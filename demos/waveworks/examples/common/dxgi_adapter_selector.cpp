#include "dxgi_adapter_selector.h"

#include <dxgi1_2.h>

#include <cstdio>

namespace
{
std::string HResultError(const char* operation, HRESULT result)
{
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s failed with HRESULT 0x%08lx",
        operation, static_cast<unsigned long>(result));
    return buffer;
}
}

bool ValidateAdapterIndex(
    uint32_t requestedIndex,
    uint32_t adapterCount,
    std::string& error)
{
    if (requestedIndex < adapterCount)
        return true;
    error = "DXGI adapter index " + std::to_string(requestedIndex) +
        " is unavailable; detected " + std::to_string(adapterCount) + " adapter(s)";
    return false;
}

bool ResolveDxgiAdapter(
    uint32_t requestedIndex,
    Microsoft::WRL::ComPtr<IDXGIAdapter>& adapter,
    std::string& error)
{
    adapter.Reset();
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(result))
    {
        error = HResultError("CreateDXGIFactory1", result);
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> selected;
    uint32_t adapterCount = 0;
    for (;; ++adapterCount)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
        result = factory->EnumAdapters1(adapterCount, &candidate);
        if (result == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(result))
        {
            error = HResultError("IDXGIFactory1::EnumAdapters1", result);
            return false;
        }
        if (adapterCount == requestedIndex)
            selected = candidate;
    }

    if (!ValidateAdapterIndex(requestedIndex, adapterCount, error))
        return false;
    result = selected.As(&adapter);
    if (FAILED(result))
    {
        error = HResultError("IDXGIAdapter1::QueryInterface", result);
        return false;
    }
    return true;
}

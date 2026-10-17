#pragma once

#include <dxgi.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

bool ValidateAdapterIndex(
    uint32_t requestedIndex,
    uint32_t adapterCount,
    std::string& error);

bool ResolveDxgiAdapter(
    uint32_t requestedIndex,
    Microsoft::WRL::ComPtr<IDXGIAdapter>& adapter,
    std::string& error);

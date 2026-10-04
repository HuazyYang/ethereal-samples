#include "meshlets/MeshShaderMode.h"

#include <algorithm>
#include <cctype>

namespace
{
    MeshShaderMode g_MeshShaderMode = MeshShaderMode::D3D12;
}

void SetMeshShaderMode(MeshShaderMode mode)
{
    g_MeshShaderMode = mode;
}

MeshShaderMode GetMeshShaderMode()
{
    return g_MeshShaderMode;
}

const char* GetMeshShaderModeName(MeshShaderMode mode)
{
    return mode == MeshShaderMode::Nvapi ? "NVAPI (2018 Turing mesh-shader extension)" : "D3D12 (SM 6.5 mesh shaders)";
}

bool ParseMeshShaderMode(const std::string& text, MeshShaderMode& mode)
{
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });

    if (lower == "nvapi")
        mode = MeshShaderMode::Nvapi;
    else if (lower == "d3d12" || lower == "dx12")
        mode = MeshShaderMode::D3D12;
    else
        return false;
    return true;
}

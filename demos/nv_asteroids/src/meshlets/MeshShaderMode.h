#pragma once

#include <string>

// Which mesh-shading path draws the meshlet geometry and the particles.
//
// Nvapi: the original 2018 path - vs_6_0 "task"/"mesh" shaders with NVAPI shader extensions
//        (asteroids/shaders/demo/nvapi/*), PSOs from NvAPI_D3D12_CreateGraphicsPipelineState with the mesh /
//        task shader extensions, draws through the undocumented NVAPI DispatchMeshTasks entry point.
// D3D12: the SM 6.5 port (amplification / mesh shaders, nvrhi meshlet pipelines, dispatchMesh).
//
// WinMain selects the mode once after the device is created (default: Nvapi when the NVAPI mesh-shader check of
// the 2018 WinMain passes, else D3D12; "-meshShaders nvapi|d3d12" overrides it - a deviation, the 2018 binary
// had only the NVAPI path). MeshletRenderResources, MeshletShaderSet, MeshletDrawStrategy and fx::ParticleSystem
// read it when they are created.
enum class MeshShaderMode
{
    D3D12,
    Nvapi,
};

void SetMeshShaderMode(MeshShaderMode mode);
[[nodiscard]] MeshShaderMode GetMeshShaderMode();
[[nodiscard]] const char* GetMeshShaderModeName(MeshShaderMode mode);

// "nvapi" / "d3d12" (case-insensitive). Returns false for anything else.
bool ParseMeshShaderMode(const std::string& text, MeshShaderMode& mode);

#pragma once

#include <nvrhi/nvrhi.h>

#include <cstdint>

// The 2018 NVAPI (Turing, pre-DX12-Ultimate) mesh-shader entry points used by the NVAPI mesh-shader mode.
//
// Asteroids.exe: NvAPI wrappers 0x140001980 (mesh-shader support query, interface 0xA47716F8) and 0x140001AD0
// (DispatchMeshTasks, interface 0x8AB10C89) - neither interface id is in the public nvapi_interface.h, so they are
// resolved through nvapi_QueryInterface - and the PSO creation of 0x1400454A0 / 0x140067DA0 through the public
// NvAPI_D3D12_CreateGraphicsPipelineState with the extensions of NvMeshShaderPsoExt.h.
//
// Everything returns false / null when the build has no NVAPI (ASTEROIDS_WITH_NVAPI) or the device is not D3D12.

struct NvMeshPipelineDesc
{
    nvrhi::IShader* taskShader = nullptr;   // vs_6_0 "task" shader; null for the particle PSO (no task stage)
    nvrhi::IShader* meshShader = nullptr;   // vs_6_0 "mesh" shader
    nvrhi::IShader* pixelShader = nullptr;  // may be null (depth-only)

    nvrhi::RenderState renderState;
    bool independentBlendEnable = true;     // the 2018 meshlet PSOs clear it for colour passes (see MeshletShaderSet)

    // Root signature layouts; must include a layout that declares the extension UAV (g_NvidiaExt).
    nvrhi::BindingLayoutVector bindingLayouts;

    uint32_t extensionUavSlot = 7;          // meshlets u7, particles u0
    uint32_t extensionRegisterSpace = 0;

    uint32_t numThreads = 32;
    uint32_t maxVertices = 64;
    uint32_t maxPrimitives = 100;
};

// 0x140001980 as used by WinMain ("Meshlets are supported on your hardware").
bool IsNvMeshShaderSupported(nvrhi::IDevice* device);

// Builds the root signature from desc.bindingLayouts (nvrhi), creates the PSO with
// NvAPI_D3D12_CreateGraphicsPipelineState and wraps it into an nvrhi graphics pipeline, so it can be bound with
// ICommandList::setGraphicsState. Logs "Failed to create a mesh shading PSO" and returns null on failure.
nvrhi::GraphicsPipelineHandle CreateNvMeshPipeline(nvrhi::IDevice* device, const NvMeshPipelineDesc& desc,
    const nvrhi::FramebufferInfo& framebufferInfo);

// 0x140001AD0: launches 'numTasks' task groups (or mesh groups for a PSO without task shader) with the state set by
// the last setGraphicsState. Call it right after setGraphicsState.
bool NvDispatchMeshTasks(nvrhi::ICommandList* commandList, uint32_t numTasks);

#pragma once

// NVAPI D3D12 PSO extensions of the 2018 (pre-DX12-Ultimate) Turing mesh-shader pipeline, reconstructed from
// Asteroids.exe (MeshletShaderSet::CreatePipeline 0x1400454A0, particles 0x140067DA0). The public NVAPI SDK
// (external/nvapi) does not declare these; NV_PSO_EXTENSION values 10 and 11 are absent from it.
//
// The PSO is created with NvAPI_D3D12_CreateGraphicsPipelineState (interface id 0x2fc28856) from a regular
// D3D12_GRAPHICS_PIPELINE_STATE_DESC with VS = null and PS = pixel shader, plus these extensions in this order:
//   [0] NVAPI_D3D12_PSO_SET_SHADER_EXTENSION_SLOT_DESC (public; meshlets: uavSlot 7 / space 0, particles: 0 / 0)
//   [1] NvPsoMeshShaderDesc (ext 11)
//   [2] NvPsoTaskShaderDesc (ext 10)   (meshlet pipelines only; the particle PSO has no task shader)
// Field names are inferred from the values the demo writes; fields marked 'unknown' are written as shown and
// their meaning is not established. Used by meshlets/NvMeshShaderApi.cpp (NVAPI mesh-shader mode).

#include <cstdint>

namespace nvmesh2018
{
    // Meshlet limits the assets and shaders are built for (globals 0x1402D2528 / 0x1402D252C).
    constexpr uint32_t c_MeshletMaxVerts = 64;
    constexpr uint32_t c_MeshletMaxPrims = 100;

    // Task / mesh workgroup size of the 2018 PSO (also [numthreads] of the SM6.5 shaders).
    constexpr uint32_t c_MeshletGroupSize = 32;

    constexpr uint32_t c_TaskShaderDescVer = 0x10030;   // MAKE_NVAPI_VERSION(NvPsoTaskShaderDesc, 1), 48 bytes
    constexpr uint32_t c_MeshShaderDescVer = 0x10040;   // MAKE_NVAPI_VERSION(NvPsoMeshShaderDesc, 1), 64 bytes
    constexpr uint32_t c_PsoExtensionDescVer = 0x10008; // NV_PSO_EXTENSION_DESC_VER (base struct, 8 bytes)

    constexpr uint32_t c_PsoTaskShaderExtension = 10;
    constexpr uint32_t c_PsoMeshShaderExtension = 11;

    constexpr uint32_t c_MeshOutputTopologyTriangles = 3;
}

#if defined(ASTEROIDS_WITH_NVAPI) && defined(__d3d12_h__)

#include <dxgi.h>   // nvapi.h's D3D declarations need IDXGISwapChain
#include <nvapi.h>

namespace nvmesh2018
{
#pragma pack(push, 4)
    // version 0x10030 (size 48, ver 1)
    struct NvPsoTaskShaderDesc : public NVAPI_D3D12_PSO_EXTENSION_DESC
    {
        NvU32 version;              // = c_TaskShaderDescVer
        NvU64 reserved0;            // 0
        NvU32 reserved1;            // 0
        const void* pShaderBytecode;
        NvU32 bytecodeLength;
        NvU32 numThreads;           // 32 (warp-sized task group)
        NvU64 unknown40;            // 96 (unknown; also written into the mesh desc when a task shader is present)
    };

    // version 0x10040 (size 64, ver 1)
    struct NvPsoMeshShaderDesc : public NVAPI_D3D12_PSO_EXTENSION_DESC
    {
        NvU32 version;              // = c_MeshShaderDescVer
        NvU64 reserved0;            // 0
        NvU32 reserved1;            // 0
        const void* pShaderBytecode;
        NvU32 bytecodeLength;
        NvU32 numThreads;           // 32
        NvU64 unknown40;            // meshlets 96, particles (no task shader) 0
        NvU32 unknown48;            // meshlets 1, particles 0 (looks like "has task shader")
        NvU32 outputTopology;       // 3 (triangles)
        NvU32 maxVertices;          // meshlets 64  (g_MeshletMaxVerts), particles 96
        NvU32 maxPrimitives;        // meshlets 100 (g_MeshletMaxPrims), particles 32
    };
#pragma pack(pop)

    static_assert(sizeof(NvPsoTaskShaderDesc) == 48, "layout must match the 2018 driver ABI");
    static_assert(sizeof(NvPsoMeshShaderDesc) == 64, "layout must match the 2018 driver ABI");
}

#endif

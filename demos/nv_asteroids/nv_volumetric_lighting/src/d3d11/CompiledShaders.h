// CompiledShaders.h
//
// Embedded shader permutation tables. The definitions are generated at build time
// (CMakeLists.txt compiles every permutation listed in shaders/Permutations.cmake with fxc /Fh and
// writes CompiledShaders.cpp). Each table has the same size and index layout as the corresponding
// table in the original DLL; entries of permutations that do not exist are nullptr.

#pragma once

#include <stdint.h>

namespace Nv
{
namespace VolumetricLighting
{
namespace Shaders
{

#define NVVL_DECLARE_SHADER_TABLE(NAME, SIZE)   \
    const uint32_t NAME##_Count = SIZE;         \
    extern const void* const NAME[SIZE];        \
    extern const uint32_t NAME##_Size[SIZE];

NVVL_DECLARE_SHADER_TABLE(ps_Apply, 32)                 // orig. table 0x1801FF3C0, sizes 0x1801FA2C0
NVVL_DECLARE_SHADER_TABLE(cs_ComputeLightLUT, 16)       // orig. table 0x1801FE250, sizes 0x1800AD750
NVVL_DECLARE_SHADER_TABLE(ps_ComputePhaseLookup, 1)     // orig. table 0x1801FF2E0, sizes 0x18001B198
NVVL_DECLARE_SHADER_TABLE(ps_Debug, 1)                  // orig. table 0x1801FE040, sizes 0x18001150C
NVVL_DECLARE_SHADER_TABLE(ps_DownsampleDepth, 2)        // orig. table 0x1801FF370, sizes 0x18001E5C8
NVVL_DECLARE_SHADER_TABLE(vs_Quad, 1)                   // orig. table 0x1801FF2E8, sizes 0x1800219F8
NVVL_DECLARE_SHADER_TABLE(vs_RenderVolume, 8)           // orig. table 0x1801FF380, sizes 0x1801DF400
NVVL_DECLARE_SHADER_TABLE(hs_RenderVolume, 64)          // orig. table 0x1801FE050, sizes 0x180097FA0
NVVL_DECLARE_SHADER_TABLE(ds_RenderVolume, 16)          // orig. table 0x1801FF2F0, sizes 0x1801D6980
NVVL_DECLARE_SHADER_TABLE(ps_RenderVolume, 512)         // orig. table 0x1801FE2E0, sizes 0x1801AEF40
NVVL_DECLARE_SHADER_TABLE(ps_Resolve, 2)                // orig. table 0x1801FE2D0, sizes 0x180014938
NVVL_DECLARE_SHADER_TABLE(ps_TemporalFilter, 1)         // orig. table 0x1801FE048, sizes 0x180017D68

#undef NVVL_DECLARE_SHADER_TABLE

} // namespace Shaders
} // namespace VolumetricLighting
} // namespace Nv

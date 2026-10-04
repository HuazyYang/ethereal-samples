// ShaderPermutations.h
//
// Permutation keys of the embedded shader tables. Each key is a bit field whose raw value is the
// index into the corresponding table (see shaders/permutations/*.json for the macro values).
// The original code uses one small struct per table with a zeroing constructor and a conversion
// operator returning the raw key (e.g. Apply_PS: ctor 0x180005920, key 0x180005940).

#pragma once

#include <stdint.h>

namespace Nv
{
namespace VolumetricLighting
{

namespace PermutationValue
{
enum : uint32_t
{
    SAMPLEMODE_SINGLE = 0,
    SAMPLEMODE_MSAA = 1,

    LIGHTMODE_DIRECTIONAL = 0,
    LIGHTMODE_SPOTLIGHT = 1,
    LIGHTMODE_OMNI = 2,

    PASSMODE_GEOMETRY = 0,          // light volume geometry (front/back faces, stencil counting)
    PASSMODE_SKY = 1,               // fullscreen pass for pixels where the volume reaches the far plane
    PASSMODE_FINAL = 2,             // fullscreen pass resolving the stencil-marked pixels

    MESHMODE_FRUSTUM_GRID = 0,      // tessellated grid covering the light frustum far plane
    MESHMODE_FRUSTUM_BASE = 1,      // two triangles closing the frustum (directional)
    MESHMODE_FRUSTUM_CAP = 2,       // spotlight cap
    MESHMODE_OMNI_VOLUME = 3,       // six tessellated faces (dual paraboloid)
    MESHMODE_GEOMETRY = 4,          // vertex-buffer geometry (compiled but not used by the D3D11 backend)

    SHADOWMAPTYPE_ATLAS = 0,
    SHADOWMAPTYPE_ARRAY = 1,

    VOLUMETYPE_FRUSTUM = 0,
    VOLUMETYPE_PARABOLOID = 1,

    UPSAMPLEMODE_POINT = 0,
    UPSAMPLEMODE_BILINEAR = 1,
    UPSAMPLEMODE_BILATERAL = 2,

    FOGMODE_NONE = 0,
    FOGMODE_NOSKY = 1,
    FOGMODE_FULL = 2,

    COMPUTEPASS_SCATTER = 0,        // pass 0: per-sample in-scattering + per-row prefix sum
    COMPUTEPASS_SUM = 1,            // pass 1: carry the row sums across the 32-texel blocks

    LUTMODE_POINT = 0,              // cs_ComputeLightLUT bit0: point LUT only
    LUTMODE_SPOTLIGHT = 1,          // cs_ComputeLightLUT bit0: point + spotlight S1/S2 LUTs
};
}

#define NVVL_PERMUTATION_KEY(NAME, FIELDS)          \
    struct NAME                                     \
    {                                               \
        union                                       \
        {                                           \
            struct                                  \
            {                                       \
                FIELDS                              \
            };                                      \
            uint32_t key_;                          \
        };                                          \
        NAME() : key_(0) {}                         \
        operator uint32_t() const { return key_; }  \
    };

// table ps_Apply @ 0x1801FF3C0 (32 entries)
NVVL_PERMUTATION_KEY(Apply_PS_Desc,
    uint32_t SAMPLEMODE : 1;
    uint32_t UPSAMPLEMODE : 2;
    uint32_t FOGMODE : 2;)

// table cs_ComputeLightLUT @ 0x1801FE250 (16 entries)
NVVL_PERMUTATION_KEY(ComputeLightLUT_CS_Desc,
    uint32_t LIGHTMODE : 1;
    uint32_t ATTENUATIONMODE : 2;
    uint32_t COMPUTEPASS : 1;)

// table vs_RenderVolume @ 0x1801FF380 (8 entries)
NVVL_PERMUTATION_KEY(RenderVolume_VS_Desc,
    uint32_t MESHMODE : 3;)

// table hs_RenderVolume @ 0x1801FE050 (64 entries)
NVVL_PERMUTATION_KEY(RenderVolume_HS_Desc,
    uint32_t SHADOWMAPTYPE : 1;
    uint32_t CASCADECOUNT : 2;
    uint32_t VOLUMETYPE : 1;
    uint32_t MAXTESSFACTOR : 2;)

// table ds_RenderVolume @ 0x1801FF2F0 (16 entries)
NVVL_PERMUTATION_KEY(RenderVolume_DS_Desc,
    uint32_t SHADOWMAPTYPE : 1;
    uint32_t CASCADECOUNT : 2;
    uint32_t VOLUMETYPE : 1;)

// table ps_RenderVolume @ 0x1801FE2E0 (512 entries)
NVVL_PERMUTATION_KEY(RenderVolume_PS_Desc,
    uint32_t SAMPLEMODE : 1;
    uint32_t LIGHTMODE : 2;
    uint32_t PASSMODE : 2;
    uint32_t ATTENUATIONMODE : 2;
    uint32_t FALLOFFMODE : 2;)

// table ps_Resolve @ 0x1801FE2D0 (2 entries)
NVVL_PERMUTATION_KEY(Resolve_PS_Desc,
    uint32_t SAMPLEMODE : 1;)

#undef NVVL_PERMUTATION_KEY

// table ps_DownsampleDepth @ 0x1801FF370 (2 entries); this key is built from the MSAA flag.
// NvVolumetricLighting.d3d11.dll: ctor 0x180005980, key 0x1800059C0
struct DownsampleDepth_PS_Desc
{
    union
    {
        struct
        {
            uint32_t SAMPLEMODE : 1;
        };
        uint32_t key_;
    };
    explicit DownsampleDepth_PS_Desc(bool msaa) : key_(0) { SAMPLEMODE = msaa; }
    operator uint32_t() const { return key_; }
};

} // namespace VolumetricLighting
} // namespace Nv

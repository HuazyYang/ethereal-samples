#ifndef SHADERS_USERDEFINED_USERDEFINEDPRESET_HLSLI
#define SHADERS_USERDEFINED_USERDEFINEDPRESET_HLSLI

#define VXGI_RESOURCE_SPACE                           4
#define VXGI_VOXELIZE_MATERIAL_CB_SLOT                6
#define VXGI_VOXELIZE_CB_SLOT                         7

#define VXGI_SCISSOR_REGIONS_SRV_SLOT                 24
#define VXGI_IRRADIANCE_MAP_SRV_SLOT                  25
#define VXGI_INVALIDATE_BITMAP_SRV_SLOT               26
#define VXGI_COVERAGE_MASKS_SRV_SLOT                  27

#define VXGI_ALLOCATION_MAP_UAV_SLOT                  8
#define VXGI_EMITTANCE_EVEN_R_UAV_SLOT                9
#define VXGI_EMITTANCE_EVEN_G_UAV_SLOT                10
#define VXGI_EMITTANCE_EVEN_B_UAV_SLOT                11
#define VXGI_EMITTANCE_ODD_R_UAV_SLOT                 12
#define VXGI_EMITTANCE_ODD_G_UAV_SLOT                 13
#define VXGI_EMITTANCE_ODD_B_UAV_SLOT                 14
#define VXGI_COVERAGE_POS_UAV_SLOT                    15
#define VXGI_COVERAGE_NEG_UAV_SLOT                    16
#define VXGI_SCISSOR_STATS_UAV_SLOT                   17

#define VXGI_IRRADIANCE_MAP_SAMPLER_SLOT              5

#endif /* SHADERS_USERDEFINED_USERDEFINEDPRESET_HLSLI */

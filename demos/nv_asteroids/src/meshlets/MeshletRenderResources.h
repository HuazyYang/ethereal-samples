#pragma once

#include <nvrhi/nvrhi.h>

// Asteroids.exe: MeshletRenderResources (0x68 bytes; ctor 0x140040680). One instance per scene, created in
// FeatureDemo::SceneLoaded (+232) and shared by the five MeshletDrawStrategy instances.
//
// The binding layouts are created lazily by MeshletDrawStrategy the first time an object / asteroid type /
// sector binding set is needed (as in 2018, where these three slots start out null).
class MeshletRenderResources
{
public:
    explicit MeshletRenderResources(nvrhi::IDevice* device);

    nvrhi::BindingLayoutHandle basicObjectBindingLayout;        // +0   space1, per SpaceObject (basicTS / basicMS)
    nvrhi::BindingLayoutHandle asteroidTypeBindingLayout;       // +8   space1, per asteroid type (asteroidTS / asteroidMS)
    nvrhi::BindingLayoutHandle asteroidSectorBindingLayout;     // +16  space1, per sector and asteroid type (t11)

    nvrhi::BufferHandle frameConstants;                 // +24 "FrameRendererConstants"   cbFrame        b0  (FrameCB, 560)
    nvrhi::BufferHandle meshletInfoConstants;           // +32 "MeshletInfoConstants"     cbMeshletInfo  b1  (8)
    nvrhi::BufferHandle instanceConstants;              // +40 "InstanceConstants"        cbInstance     b2  (48)
    nvrhi::BufferHandle instanceConstantsPrevious;      // +48 "InstanceConstantsPrevious" cbInstancePrev b3 (48)
    nvrhi::BufferHandle sectorConstants;                // +56 "SectorConstants"          cbSectorInfo   b5  (16)
    nvrhi::BufferHandle objectConstants;                // +64 "ObjectConstants"          cbObjectInfo   b4  (384, space objects)
    nvrhi::BufferHandle debugUAV;                       // +72 "DebugUAV"  u0 (0x10000 x 20 bytes)
    nvrhi::BufferHandle statsUAV;                       // +80 "StatsUAV"  u1 (u_Stats, 16 x uint)
    nvrhi::SamplerHandle linearClampSampler;            // +88 s0 s_ViewDistanceSampler
    nvrhi::SamplerHandle maxReductionSampler;           // +96 s1 s_ZFarSampler (MAXIMUM reduction for the Hi-Z lookup)

    // deviation: the 2018 nvrhi turned a null texture binding into a null SRV (reads 0); the light-probe
    // capture (0x1400327F0) passes a null Hi-Z texture. Current nvrhi requires a resource, so null
    // optional textures are replaced by this 1x1 zero texture.
    nvrhi::TextureHandle nullTexture;

    // NVAPI mesh-shader mode only (null otherwise): the NVAPI shader-extension UAV g_NvidiaExt (u7, space0).
    // 2018: every meshlet binding layout carried an extra item 0x00030007 (UAV u7) with a null resource, so the
    // slot was part of the root signature built by 0x14019B5E0. nvrhi layouts have one register space each, so the
    // slot is a separate space0 layout appended after the meshlet layouts, bound to a 1-element dummy buffer
    // (the driver intercepts the accesses; the buffer is never written).
    nvrhi::BufferHandle nvExtensionBuffer;
    nvrhi::BindingLayoutHandle nvExtensionBindingLayout;
    nvrhi::BindingSetHandle nvExtensionBindingSet;
};

namespace nvmesh2018
{
    // The NvShaderExtnStruct stride (nvShaderExtnEnums.h / nvHLSLExtns.h).
    constexpr uint32_t c_ShaderExtnStructSize = 256;

    // Layout + set of a single StructuredBuffer_UAV at 'slot' / 'registerSpace' for the NVAPI extension UAV.
    bool CreateExtensionBinding(nvrhi::IDevice* device, uint32_t slot, uint32_t registerSpace,
        nvrhi::BufferHandle& buffer, nvrhi::BindingLayoutHandle& layout, nvrhi::BindingSetHandle& set);
}

// ContextImp_D3D11.h
//
// D3D11 backend of the volumetric lighting context.

#pragma once

#include "../ContextImp.h"
#include "D3D11Util.h"

#include <d3d11.h>

namespace Nv
{
namespace VolumetricLighting
{

// NvVolumetricLighting.d3d11.dll: vtable 0x1801FB898, sizeof == 728
class ContextImp_D3D11 : public ContextImp
{
public:
    // Allocates the context and all its device objects.
    // NvVolumetricLighting.d3d11.dll: 0x180005EA0
    static Status Create(ContextImp_D3D11** out_ctx, const PlatformDesc* pPlatformDesc, const ContextDesc* pContextDesc);

    explicit ContextImp_D3D11(const ContextDesc* pContextDesc);
    ~ContextImp_D3D11() override;

protected:
    Status CreateResources(ID3D11Device* device);

    Status BeginAccumulation_Start(BeginAccumulationArgs* pArgs) override;
    Status BeginAccumulation_UpdateMediumLUT(BeginAccumulationArgs* pArgs) override;
    Status BeginAccumulation_CopyDepth(BeginAccumulationArgs* pArgs) override;
    Status BeginAccumulation_End(BeginAccumulationArgs* pArgs) override;
    Status RenderVolume_Start(RenderVolumeArgs* pArgs) override;
    Status RenderVolume_DoVolume_Directional(RenderVolumeArgs* pArgs) override;
    Status RenderVolume_DoVolume_Spotlight(RenderVolumeArgs* pArgs) override;
    Status RenderVolume_DoVolume_Omni(RenderVolumeArgs* pArgs) override;
    Status RenderVolume_End(RenderVolumeArgs* pArgs) override;
    Status EndAccumulation_Imp(EndAccumulationArgs* pArgs) override;
    Status ApplyLighting_Start(ApplyLightingArgs* pArgs) override;
    Status ApplyLighting_Resolve(ApplyLightingArgs* pArgs) override;
    Status ApplyLighting_TemporalFilter(ApplyLightingArgs* pArgs) override;
    Status ApplyLighting_Composite(ApplyLightingArgs* pArgs) override;
    Status ApplyLighting_End(ApplyLightingArgs* pArgs) override;

    // Draw helpers
    Status DrawFullscreen(ID3D11DeviceContext* dxCtx);
    Status DrawFrustumGrid(ID3D11DeviceContext* dxCtx, uint32_t resolution);
    Status DrawFrustumBase(ID3D11DeviceContext* dxCtx, uint32_t resolution);
    Status DrawFrustumCap(ID3D11DeviceContext* dxCtx, uint32_t resolution);
    Status DrawOmniVolume(ID3D11DeviceContext* dxCtx, uint32_t resolution);

    // Shader permutation tables (+328 .. +416)
    ID3D11PixelShader** shaders_Apply_PS_;
    ID3D11ComputeShader** shaders_ComputeLightLUT_CS_;
    ID3D11PixelShader** shaders_ComputePhaseLookup_PS_;
    ID3D11PixelShader** shaders_Debug_PS_;
    ID3D11PixelShader** shaders_DownsampleDepth_PS_;
    ID3D11VertexShader** shaders_Quad_VS_;
    ID3D11VertexShader** shaders_RenderVolume_VS_;
    ID3D11HullShader** shaders_RenderVolume_HS_;
    ID3D11DomainShader** shaders_RenderVolume_DS_;
    ID3D11PixelShader** shaders_RenderVolume_PS_;
    ID3D11PixelShader** shaders_Resolve_PS_;
    ID3D11PixelShader** shaders_TemporalFilter_PS_;

    // Constant buffers (+424 .. +448)
    ConstantBuffer<PerContextCB>* pPerContextCB_;
    ConstantBuffer<PerFrameCB>* pPerFrameCB_;
    ConstantBuffer<PerVolumeCB>* pPerVolumeCB_;
    ConstantBuffer<PerApplyCB>* pPerApplyCB_;

    // Render targets (+456 .. +576)
    DepthTarget* pDepth_;                       // "NvVl::Depth": internal depth/stencil (D24S8, internal MSAA)
    RenderTarget* pPhaseLUT_;                   // "NvVl::Phase LUT": 1x512 RGBA16F
    RenderTarget* pLightLUT_P_[2];              // "NvVl::Light LUT Point [i]": 256x512 RGBA16F
    RenderTarget* pLightLUT_S1_[2];             // "NvVl::Light LUT Spot 1 [i]"
    RenderTarget* pLightLUT_S2_[2];             // "NvVl::Light LUT Spot 2 [i]"
    RenderTarget* pAccumulation_;               // "NvVl::Accumulation": RGBA16F, internal MSAA
    RenderTarget* pResolvedAccumulation_;       // "NvVl::Resolved Accumulation" (MSAA or temporal only)
    RenderTarget* pResolvedDepth_;              // "NvVl::Resolved Depth": RG16F (MSAA or temporal only)
    RenderTarget* pFilteredAccumulation_[2];    // "NvVl::Filtered Accumulation" (temporal only)
    RenderTarget* pFilteredDepth_[2];           // "NvVl::Filtered Depth" (temporal only)
    RenderTarget* pAccumulatedOutput_;          // accumulation result consumed by the composite pass

    // Device states (+584 .. +720)
    ID3D11RasterizerState* rs_CullNone_;
    ID3D11RasterizerState* rs_CullFront_;
    ID3D11RasterizerState* rs_Wireframe_;
    ID3D11SamplerState* ss_Point_;
    ID3D11SamplerState* ss_Linear_;
    ID3D11DepthStencilState* dss_NoDepth_;
    ID3D11DepthStencilState* dss_WriteDepth_;
    ID3D11DepthStencilState* dss_TestDepth_;            // created but not used by the D3D11 backend
    ID3D11DepthStencilState* dss_RenderVolume_;
    ID3D11DepthStencilState* dss_RenderVolume_Sky_;
    ID3D11DepthStencilState* dss_Unused_;               // unresolved: never created, released or used (+664)
    ID3D11DepthStencilState* dss_RenderVolume_NoDepth_; // created but not used by the D3D11 backend
    ID3D11DepthStencilState* dss_RenderVolume_Final_;
    ID3D11BlendState* bs_NoColor_;
    ID3D11BlendState* bs_NoBlend_;
    ID3D11BlendState* bs_Additive_;
    ID3D11BlendState* bs_Additive_Modulate_;
    ID3D11BlendState* bs_Debug_Blend_;
};

} // namespace VolumetricLighting
} // namespace Nv

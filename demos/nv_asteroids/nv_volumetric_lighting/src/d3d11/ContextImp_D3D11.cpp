// ContextImp_D3D11.cpp
//
// D3D11 backend: device object creation, accumulation passes (phase LUT, depth copy, light volume
// rendering for directional / spot / point lights, light LUT compute) and the apply passes
// (resolve, temporal filter, composite).

#include "ContextImp_D3D11.h"
#include "CompiledShaders.h"
#include "ShaderPermutations.h"

namespace Nv
{
namespace VolumetricLighting
{

using namespace PermutationValue;

namespace
{

const uint32_t DEBUG_WIREFRAME = static_cast<uint32_t>(DebugFlags::WIREFRAME);
const uint32_t DEBUG_NO_BLENDING = static_cast<uint32_t>(DebugFlags::NO_BLENDING);

const DXGI_FORMAT ACCUMULATION_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;   // 10
const DXGI_FORMAT RESOLVED_DEPTH_FORMAT = DXGI_FORMAT_R16G16_FLOAT;       // 34
const DXGI_FORMAT DEPTH_FORMAT = DXGI_FORMAT_D24_UNORM_S8_UINT;           // 45

const uint32_t PHASE_LUT_RESOLUTION = 512;
const uint32_t LIGHT_LUT_WIDTH = 256;
const uint32_t LIGHT_LUT_HEIGHT = 512;

} // namespace

////////////////////////////////////////////////////////////////////////////////
// Creation / destruction

// NvVolumetricLighting.d3d11.dll: 0x180005EA0
Status ContextImp_D3D11::Create(ContextImp_D3D11** out_ctx, const PlatformDesc* pPlatformDesc,
                                const ContextDesc* pContextDesc)
{
    ID3D11Device* device = pPlatformDesc->d3d11.pDevice;
    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
    {
        return Status::UNSUPPORTED_DEVICE;
    }

    *out_ctx = NV_NEW ContextImp_D3D11(pContextDesc);
    Status status = (*out_ctx)->CreateResources(device);
    if (status == Status::OK)
    {
        return Status::OK;
    }
    SAFE_DELETE(*out_ctx);
    return status;
}

// NvVolumetricLighting.d3d11.dll: 0x180007980
ContextImp_D3D11::ContextImp_D3D11(const ContextDesc* pContextDesc)
    : ContextImp(pContextDesc)
{
    shaders_Apply_PS_ = nullptr;
    shaders_ComputeLightLUT_CS_ = nullptr;
    shaders_ComputePhaseLookup_PS_ = nullptr;
    shaders_Debug_PS_ = nullptr;
    shaders_DownsampleDepth_PS_ = nullptr;
    shaders_Quad_VS_ = nullptr;
    shaders_RenderVolume_VS_ = nullptr;
    shaders_RenderVolume_HS_ = nullptr;
    shaders_RenderVolume_DS_ = nullptr;
    shaders_RenderVolume_PS_ = nullptr;
    shaders_Resolve_PS_ = nullptr;
    shaders_TemporalFilter_PS_ = nullptr;
    pPerContextCB_ = nullptr;
    pPerFrameCB_ = nullptr;
    pPerVolumeCB_ = nullptr;
    pPerApplyCB_ = nullptr;
    pDepth_ = nullptr;
    pPhaseLUT_ = nullptr;
    pLightLUT_P_[0] = nullptr;
    pLightLUT_P_[1] = nullptr;
    pLightLUT_S1_[0] = nullptr;
    pLightLUT_S1_[1] = nullptr;
    pLightLUT_S2_[0] = nullptr;
    pLightLUT_S2_[1] = nullptr;
    pAccumulation_ = nullptr;
    pResolvedAccumulation_ = nullptr;
    pResolvedDepth_ = nullptr;
    for (int i = 0; i < 2; ++i)
    {
        pFilteredAccumulation_[i] = nullptr;
        pFilteredDepth_[i] = nullptr;
    }
    // pAccumulatedOutput_ and dss_Unused_ are not initialized by the original constructor
    rs_CullNone_ = nullptr;
    rs_CullFront_ = nullptr;
    rs_Wireframe_ = nullptr;
    ss_Point_ = nullptr;
    ss_Linear_ = nullptr;
    dss_NoDepth_ = nullptr;
    dss_WriteDepth_ = nullptr;
    dss_TestDepth_ = nullptr;
    dss_RenderVolume_ = nullptr;
    dss_RenderVolume_Sky_ = nullptr;
    dss_RenderVolume_NoDepth_ = nullptr;
    dss_RenderVolume_Final_ = nullptr;
    bs_NoColor_ = nullptr;
    bs_NoBlend_ = nullptr;
    bs_Additive_ = nullptr;
    bs_Additive_Modulate_ = nullptr;
    bs_Debug_Blend_ = nullptr;
}

// NvVolumetricLighting.d3d11.dll: 0x180007D00 (deleting variant 0x1800058E0)
ContextImp_D3D11::~ContextImp_D3D11()
{
#define RELEASE_SHADER_TABLE(x) \
    if ((x) != nullptr)         \
    {                           \
        SAFE_RELEASE_ARRAY(x);  \
        delete[] (x);           \
        (x) = nullptr;          \
    }

    RELEASE_SHADER_TABLE(shaders_Apply_PS_);
    RELEASE_SHADER_TABLE(shaders_ComputeLightLUT_CS_);
    RELEASE_SHADER_TABLE(shaders_ComputePhaseLookup_PS_);
    RELEASE_SHADER_TABLE(shaders_Debug_PS_);
    RELEASE_SHADER_TABLE(shaders_DownsampleDepth_PS_);
    RELEASE_SHADER_TABLE(shaders_Quad_VS_);
    RELEASE_SHADER_TABLE(shaders_RenderVolume_VS_);
    RELEASE_SHADER_TABLE(shaders_RenderVolume_HS_);
    RELEASE_SHADER_TABLE(shaders_RenderVolume_DS_);
    RELEASE_SHADER_TABLE(shaders_RenderVolume_PS_);
    RELEASE_SHADER_TABLE(shaders_Resolve_PS_);
    RELEASE_SHADER_TABLE(shaders_TemporalFilter_PS_);
#undef RELEASE_SHADER_TABLE

    SAFE_DELETE(pPerContextCB_);
    SAFE_DELETE(pPerFrameCB_);
    SAFE_DELETE(pPerVolumeCB_);
    SAFE_DELETE(pPerApplyCB_);
    SAFE_DELETE(pDepth_);
    SAFE_DELETE(pPhaseLUT_);
    SAFE_DELETE(pLightLUT_P_[0]);
    SAFE_DELETE(pLightLUT_P_[1]);
    SAFE_DELETE(pLightLUT_S1_[0]);
    SAFE_DELETE(pLightLUT_S1_[1]);
    SAFE_DELETE(pLightLUT_S2_[0]);
    SAFE_DELETE(pLightLUT_S2_[1]);
    SAFE_DELETE(pAccumulation_);
    SAFE_DELETE(pResolvedAccumulation_);
    SAFE_DELETE(pResolvedDepth_);
    for (int i = 0; i < 2; ++i)
    {
        SAFE_DELETE(pFilteredAccumulation_[i]);
        SAFE_DELETE(pFilteredDepth_[i]);
    }

    SAFE_RELEASE(rs_CullNone_);
    SAFE_RELEASE(rs_CullFront_);
    SAFE_RELEASE(rs_Wireframe_);
    SAFE_RELEASE(ss_Point_);
    SAFE_RELEASE(ss_Linear_);
    SAFE_RELEASE(dss_NoDepth_);
    SAFE_RELEASE(dss_WriteDepth_);
    SAFE_RELEASE(dss_TestDepth_);
    SAFE_RELEASE(dss_RenderVolume_);
    SAFE_RELEASE(dss_RenderVolume_Sky_);
    SAFE_RELEASE(dss_RenderVolume_NoDepth_);
    SAFE_RELEASE(dss_RenderVolume_Final_);
    SAFE_RELEASE(bs_NoColor_);
    SAFE_RELEASE(bs_NoBlend_);
    SAFE_RELEASE(bs_Additive_);
    SAFE_RELEASE(bs_Additive_Modulate_);
    SAFE_RELEASE(bs_Debug_Blend_);
}

// NvVolumetricLighting.d3d11.dll: 0x180005FE0
Status ContextImp_D3D11::CreateResources(ID3D11Device* device)
{
    // Shaders
#define LOAD_SHADERS(TABLE, MEMBER)                                                                       \
    if (FAILED(LoadShaders(device, Shaders::TABLE, Shaders::TABLE##_Size, Shaders::TABLE##_Count, MEMBER))) \
    {                                                                                                     \
        return Status::API_ERROR;                                                                         \
    }

    LOAD_SHADERS(ps_Apply, shaders_Apply_PS_);
    LOAD_SHADERS(cs_ComputeLightLUT, shaders_ComputeLightLUT_CS_);
    LOAD_SHADERS(ps_ComputePhaseLookup, shaders_ComputePhaseLookup_PS_);
    LOAD_SHADERS(ps_Debug, shaders_Debug_PS_);
    LOAD_SHADERS(ps_DownsampleDepth, shaders_DownsampleDepth_PS_);
    LOAD_SHADERS(vs_Quad, shaders_Quad_VS_);
    LOAD_SHADERS(vs_RenderVolume, shaders_RenderVolume_VS_);
    LOAD_SHADERS(hs_RenderVolume, shaders_RenderVolume_HS_);
    LOAD_SHADERS(ds_RenderVolume, shaders_RenderVolume_DS_);
    LOAD_SHADERS(ps_RenderVolume, shaders_RenderVolume_PS_);
    LOAD_SHADERS(ps_Resolve, shaders_Resolve_PS_);
    LOAD_SHADERS(ps_TemporalFilter, shaders_TemporalFilter_PS_);
#undef LOAD_SHADERS

    // Constant buffers
    pPerContextCB_ = ConstantBuffer<PerContextCB>::Create(device);
    if (pPerContextCB_ == nullptr)
    {
        return Status::RESOURCE_FAILURE;
    }
    pPerFrameCB_ = ConstantBuffer<PerFrameCB>::Create(device);
    if (pPerFrameCB_ == nullptr)
    {
        return Status::RESOURCE_FAILURE;
    }
    pPerVolumeCB_ = ConstantBuffer<PerVolumeCB>::Create(device);
    if (pPerVolumeCB_ == nullptr)
    {
        return Status::RESOURCE_FAILURE;
    }
    pPerApplyCB_ = ConstantBuffer<PerApplyCB>::Create(device);
    if (pPerApplyCB_ == nullptr)
    {
        return Status::RESOURCE_FAILURE;
    }

    // Render targets
#define CREATE_TARGET(MEMBER, EXPR)          \
    MEMBER = (EXPR);                         \
    if ((MEMBER) == nullptr)                 \
    {                                        \
        return Status::RESOURCE_FAILURE;     \
    }

    CREATE_TARGET(pDepth_, DepthTarget::Create(device, GetInternalBufferWidth(), GetInternalBufferHeight(),
                                               GetInternalSampleCount(), DEPTH_FORMAT, 1, "NvVl::Depth"));
    CREATE_TARGET(pPhaseLUT_, RenderTarget::Create(device, 1, PHASE_LUT_RESOLUTION, 1, ACCUMULATION_FORMAT,
                                                   "NvVl::Phase LUT"));
    CREATE_TARGET(pLightLUT_P_[0], RenderTarget::Create(device, LIGHT_LUT_WIDTH, LIGHT_LUT_HEIGHT, 1, ACCUMULATION_FORMAT,
                                                        "NvVl::Light LUT Point [0]"));
    CREATE_TARGET(pLightLUT_P_[1], RenderTarget::Create(device, LIGHT_LUT_WIDTH, LIGHT_LUT_HEIGHT, 1, ACCUMULATION_FORMAT,
                                                        "NvVl::Light LUT Point [1]"));
    CREATE_TARGET(pLightLUT_S1_[0], RenderTarget::Create(device, LIGHT_LUT_WIDTH, LIGHT_LUT_HEIGHT, 1, ACCUMULATION_FORMAT,
                                                         "NvVl::Light LUT Spot 1 [0]"));
    CREATE_TARGET(pLightLUT_S1_[1], RenderTarget::Create(device, LIGHT_LUT_WIDTH, LIGHT_LUT_HEIGHT, 1, ACCUMULATION_FORMAT,
                                                         "NvVl::Light LUT Spot 1 [1]"));
    CREATE_TARGET(pLightLUT_S2_[0], RenderTarget::Create(device, LIGHT_LUT_WIDTH, LIGHT_LUT_HEIGHT, 1, ACCUMULATION_FORMAT,
                                                         "NvVl::Light LUT Spot 2 [0]"));
    CREATE_TARGET(pLightLUT_S2_[1], RenderTarget::Create(device, LIGHT_LUT_WIDTH, LIGHT_LUT_HEIGHT, 1, ACCUMULATION_FORMAT,
                                                         "NvVl::Light LUT Spot 2 [1]"));
    CREATE_TARGET(pAccumulation_, RenderTarget::Create(device, GetInternalBufferWidth(), GetInternalBufferHeight(),
                                                       GetInternalSampleCount(), ACCUMULATION_FORMAT, "NvVl::Accumulation"));

    if (IsInternalMSAA() || GetFilterMode() == FilterMode::TEMPORAL)
    {
        CREATE_TARGET(pResolvedAccumulation_, RenderTarget::Create(device, GetInternalBufferWidth(), GetInternalBufferHeight(),
                                                                   1, ACCUMULATION_FORMAT, "NvVl::Resolved Accumulation"));
        CREATE_TARGET(pResolvedDepth_, RenderTarget::Create(device, GetInternalBufferWidth(), GetInternalBufferHeight(),
                                                            1, RESOLVED_DEPTH_FORMAT, "NvVl::Resolved Depth"));
    }

    if (GetFilterMode() == FilterMode::TEMPORAL)
    {
        for (int i = 0; i < 2; ++i)
        {
            CREATE_TARGET(pFilteredDepth_[i], RenderTarget::Create(device, GetInternalBufferWidth(), GetInternalBufferHeight(),
                                                                   1, RESOLVED_DEPTH_FORMAT, "NvVl::Filtered Depth"));
            CREATE_TARGET(pFilteredAccumulation_[i], RenderTarget::Create(device, GetInternalBufferWidth(),
                                                                          GetInternalBufferHeight(), 1,
                                                                          ACCUMULATION_FORMAT, "NvVl::Filtered Accumulation"));
        }
    }
#undef CREATE_TARGET

    // Rasterizer states: solid/cull none, solid/cull front, wireframe. Depth clipping is disabled
    // so the light volumes are not clipped by the camera far plane.
    {
        CD3D11_RASTERIZER_DESC rsDesc((CD3D11_DEFAULT()));
        rsDesc.FrontCounterClockwise = TRUE;
        rsDesc.CullMode = D3D11_CULL_NONE;
        rsDesc.DepthClipEnable = FALSE;
        if (FAILED(device->CreateRasterizerState(&rsDesc, &rs_CullNone_)))
        {
            return Status::API_ERROR;
        }
    }
    {
        CD3D11_RASTERIZER_DESC rsDesc((CD3D11_DEFAULT()));
        rsDesc.FrontCounterClockwise = TRUE;
        rsDesc.CullMode = D3D11_CULL_FRONT;
        rsDesc.DepthClipEnable = FALSE;
        if (FAILED(device->CreateRasterizerState(&rsDesc, &rs_CullFront_)))
        {
            return Status::API_ERROR;
        }
    }
    {
        CD3D11_RASTERIZER_DESC rsDesc((CD3D11_DEFAULT()));
        rsDesc.FillMode = D3D11_FILL_WIREFRAME;
        rsDesc.FrontCounterClockwise = TRUE;
        rsDesc.CullMode = D3D11_CULL_NONE;
        rsDesc.DepthClipEnable = FALSE;
        if (FAILED(device->CreateRasterizerState(&rsDesc, &rs_Wireframe_)))
        {
            return Status::API_ERROR;
        }
    }

    // Samplers: point (s0) and bilinear (s1), clamp addressing
    {
        CD3D11_SAMPLER_DESC ssDesc((CD3D11_DEFAULT()));
        ssDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        if (FAILED(device->CreateSamplerState(&ssDesc, &ss_Point_)))
        {
            return Status::API_ERROR;
        }
    }
    {
        CD3D11_SAMPLER_DESC ssDesc((CD3D11_DEFAULT()));
        ssDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        if (FAILED(device->CreateSamplerState(&ssDesc, &ss_Linear_)))
        {
            return Status::API_ERROR;
        }
    }

    // Depth-stencil states
    {
        // no depth, no stencil
        CD3D11_DEPTH_STENCIL_DESC dsDesc((CD3D11_DEFAULT()));
        dsDesc.DepthEnable = FALSE;
        if (FAILED(device->CreateDepthStencilState(&dsDesc, &dss_NoDepth_)))
        {
            return Status::API_ERROR;
        }
    }
    {
        // unconditional depth write (depth copy)
        CD3D11_DEPTH_STENCIL_DESC dsDesc((CD3D11_DEFAULT()));
        dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        if (FAILED(device->CreateDepthStencilState(&dsDesc, &dss_WriteDepth_)))
        {
            return Status::API_ERROR;
        }
    }
    {
        // light volume geometry: depth test without write; front faces failing the depth test
        // increment, back faces failing it decrement the stencil (cleared to 0xFF per volume)
        CD3D11_DEPTH_STENCIL_DESC dsDesc((CD3D11_DEFAULT()));
        dsDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dsDesc.StencilEnable = TRUE;
        dsDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        dsDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        dsDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR;
        dsDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        dsDesc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        dsDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        dsDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_DECR;
        dsDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        if (FAILED(device->CreateDepthStencilState(&dsDesc, &dss_RenderVolume_)))
        {
            return Status::API_ERROR;
        }
    }
    {
        // sky pass (directional lights): fullscreen quad at the far plane, front faces rejected,
        // back faces failing the depth test decrement the stencil
        CD3D11_DEPTH_STENCIL_DESC dsDesc((CD3D11_DEFAULT()));
        dsDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dsDesc.StencilEnable = TRUE;
        dsDesc.FrontFace.StencilFunc = D3D11_COMPARISON_NEVER;
        dsDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        dsDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        dsDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        dsDesc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        dsDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        dsDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_DECR;
        dsDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        if (FAILED(device->CreateDepthStencilState(&dsDesc, &dss_RenderVolume_Sky_)))
        {
            return Status::API_ERROR;
        }
    }
    {
        // stencil counting without depth (not used by this backend)
        CD3D11_DEPTH_STENCIL_DESC dsDesc((CD3D11_DEFAULT()));
        dsDesc.DepthEnable = FALSE;
        dsDesc.StencilEnable = TRUE;
        dsDesc.FrontFace.StencilFunc = D3D11_COMPARISON_LESS_EQUAL;
        dsDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        dsDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR;
        dsDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        dsDesc.BackFace.StencilFunc = D3D11_COMPARISON_LESS_EQUAL;
        dsDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        dsDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_DECR;
        dsDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        if (FAILED(device->CreateDepthStencilState(&dsDesc, &dss_RenderVolume_NoDepth_)))
        {
            return Status::API_ERROR;
        }
    }
    {
        // final pass: fullscreen quad (back facing) where 0xFF > stencil, stencil not modified
        CD3D11_DEPTH_STENCIL_DESC dsDesc((CD3D11_DEFAULT()));
        dsDesc.DepthEnable = FALSE;
        dsDesc.StencilEnable = TRUE;
        dsDesc.StencilWriteMask = 0x00;
        dsDesc.FrontFace.StencilFunc = D3D11_COMPARISON_NEVER;
        dsDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        dsDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        dsDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        dsDesc.BackFace.StencilFunc = D3D11_COMPARISON_GREATER;
        dsDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        dsDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        dsDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        if (FAILED(device->CreateDepthStencilState(&dsDesc, &dss_RenderVolume_Final_)))
        {
            return Status::API_ERROR;
        }
    }
    {
        // depth test without write (not used by this backend)
        CD3D11_DEPTH_STENCIL_DESC dsDesc((CD3D11_DEFAULT()));
        dsDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (FAILED(device->CreateDepthStencilState(&dsDesc, &dss_TestDepth_)))
        {
            return Status::API_ERROR;
        }
    }

    // Blend states
    {
        // depth-only rendering
        CD3D11_BLEND_DESC bsDesc((CD3D11_DEFAULT()));
        bsDesc.RenderTarget[0].RenderTargetWriteMask = 0x00;
        if (FAILED(device->CreateBlendState(&bsDesc, &bs_NoColor_)))
        {
            return Status::API_ERROR;
        }
    }
    {
        CD3D11_BLEND_DESC bsDesc((CD3D11_DEFAULT()));
        if (FAILED(device->CreateBlendState(&bsDesc, &bs_NoBlend_)))
        {
            return Status::API_ERROR;
        }
    }
    {
        // accumulation: dst += src * blend factor
        CD3D11_BLEND_DESC bsDesc((CD3D11_DEFAULT()));
        bsDesc.RenderTarget[0].BlendEnable = TRUE;
        bsDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bsDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_BLEND_FACTOR;
        bsDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        bsDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bsDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
        bsDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        if (FAILED(device->CreateBlendState(&bsDesc, &bs_Additive_)))
        {
            return Status::API_ERROR;
        }
    }
    {
        // composite: dst = src0 * blend factor + dst * src1 (dual source: in-scattering, transmittance)
        CD3D11_BLEND_DESC bsDesc((CD3D11_DEFAULT()));
        bsDesc.RenderTarget[0].BlendEnable = TRUE;
        bsDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bsDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_BLEND_FACTOR;
        bsDesc.RenderTarget[0].DestBlend = D3D11_BLEND_SRC1_COLOR;
        bsDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bsDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bsDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        if (FAILED(device->CreateBlendState(&bsDesc, &bs_Additive_Modulate_)))
        {
            return Status::API_ERROR;
        }
    }
    {
        // DebugFlags::NO_BLENDING: overwrite color with the in-scattering
        CD3D11_BLEND_DESC bsDesc((CD3D11_DEFAULT()));
        bsDesc.RenderTarget[0].BlendEnable = TRUE;
        bsDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bsDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bsDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        bsDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bsDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bsDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_SRC1_ALPHA;
        if (FAILED(device->CreateBlendState(&bsDesc, &bs_Debug_Blend_)))
        {
            return Status::API_ERROR;
        }
    }
    return Status::OK;
}

////////////////////////////////////////////////////////////////////////////////
// BeginAccumulation

// NvVolumetricLighting.d3d11.dll: 0x180009740
Status ContextImp_D3D11::BeginAccumulation_Start(BeginAccumulationArgs* pArgs)
{
    ID3D11DeviceContext* dxCtx = pArgs->renderCtx;
    const ViewerDesc* pViewerDesc = pArgs->pViewerDesc;
    const MediumDesc* pMediumDesc = pArgs->pMediumDesc;

    if (!isInitialized_)
    {
        isInitialized_ = true;
        SetupCB_PerContext(pPerContextCB_->Map(dxCtx));
        pPerContextCB_->Unmap(dxCtx);
    }
    SetupCB_PerFrame(pViewerDesc, pMediumDesc, pPerFrameCB_->Map(dxCtx));
    pPerFrameCB_->Unmap(dxCtx);

    ID3D11Buffer* constantBuffers[] = { pPerContextCB_->getCB(), pPerFrameCB_->getCB(), nullptr, nullptr };
    dxCtx->VSSetConstantBuffers(0, 4, constantBuffers);
    dxCtx->PSSetConstantBuffers(0, 4, constantBuffers);
    ID3D11SamplerState* samplers[] = { ss_Point_, ss_Linear_ };
    dxCtx->PSSetSamplers(0, 2, samplers);
    return Status::OK;
}

// Renders the phase function lookup table (1x512, indexed by scattering angle).
// NvVolumetricLighting.d3d11.dll: 0x180009920
Status ContextImp_D3D11::BeginAccumulation_UpdateMediumLUT(BeginAccumulationArgs* pArgs)
{
    ID3D11DeviceContext* dxCtx = pArgs->renderCtx;

    FLOAT black[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    dxCtx->ClearRenderTargetView(pPhaseLUT_->getRTV(), black);
    D3D11_VIEWPORT viewport = { 0.0f, 0.0f, 1.0f, (float)PHASE_LUT_RESOLUTION, 0.0f, 1.0f };
    dxCtx->RSSetViewports(1, &viewport);
    ID3D11RenderTargetView* rtv = pPhaseLUT_->getRTV();
    dxCtx->OMSetRenderTargets(1, &rtv, nullptr);
    dxCtx->OMSetDepthStencilState(dss_NoDepth_, 0);
    dxCtx->OMSetBlendState(bs_NoBlend_, nullptr, 0xFFFFFFFF);
    dxCtx->PSSetShader(shaders_ComputePhaseLookup_PS_[0], nullptr, 0);
    V_RETURN(DrawFullscreen(dxCtx));
    return Status::OK;
}

// Copies (and downsamples) the scene depth into the internal depth-stencil buffer.
// NvVolumetricLighting.d3d11.dll: 0x180009B20
Status ContextImp_D3D11::BeginAccumulation_CopyDepth(BeginAccumulationArgs* pArgs)
{
    ID3D11DeviceContext* dxCtx = pArgs->renderCtx;
    ID3D11ShaderResourceView* sceneDepth = pArgs->sceneDepth;

    dxCtx->ClearDepthStencilView(pDepth_->getDSV(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    D3D11_VIEWPORT viewport = { 0.0f, 0.0f, (float)GetInternalViewportWidth(), (float)GetInternalViewportHeight(), 0.0f, 1.0f };
    dxCtx->RSSetViewports(1, &viewport);
    dxCtx->PSSetShaderResources(0, 1, &sceneDepth);
    ID3D11RenderTargetView* nullRTV = nullptr;
    dxCtx->OMSetRenderTargets(1, &nullRTV, pDepth_->getDSV());
    dxCtx->OMSetDepthStencilState(dss_WriteDepth_, 0);
    dxCtx->OMSetBlendState(bs_NoColor_, nullptr, 0xFFFFFFFF);
    DownsampleDepth_PS_Desc psDesc(IsOutputMSAA());
    dxCtx->PSSetShader(shaders_DownsampleDepth_PS_[psDesc], nullptr, 0);
    V_RETURN(DrawFullscreen(dxCtx));
    return Status::OK;
}

// NvVolumetricLighting.d3d11.dll: 0x180009DA0
Status ContextImp_D3D11::BeginAccumulation_End(BeginAccumulationArgs* pArgs)
{
    ID3D11DeviceContext* dxCtx = pArgs->renderCtx;
    FLOAT black[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    dxCtx->ClearRenderTargetView(pAccumulation_->getRTV(), black);
    return Status::OK;
}

////////////////////////////////////////////////////////////////////////////////
// RenderVolume

// NvVolumetricLighting.d3d11.dll: 0x180009E40
Status ContextImp_D3D11::RenderVolume_Start(RenderVolumeArgs* pArgs)
{
    ID3D11DeviceContext* dxCtx = pArgs->renderCtx;
    const ShadowMapDesc* pShadowMapDesc = pArgs->pShadowMapDesc;
    const LightDesc* pLightDesc = pArgs->pLightDesc;
    const VolumeDesc* pVolumeDesc = pArgs->pVolumeDesc;

    SetupCB_PerVolume(pShadowMapDesc, pLightDesc, pVolumeDesc, pPerVolumeCB_->Map(dxCtx));
    pPerVolumeCB_->Unmap(dxCtx);

    // each volume starts with a stencil of 0xFF
    dxCtx->ClearDepthStencilView(pDepth_->getDSV(), D3D11_CLEAR_STENCIL, 1.0f, 0xFF);

    ID3D11Buffer* constantBuffers[] = { pPerContextCB_->getCB(), pPerFrameCB_->getCB(), pPerVolumeCB_->getCB(), nullptr };
    dxCtx->VSSetConstantBuffers(0, 4, constantBuffers);
    dxCtx->HSSetConstantBuffers(0, 4, constantBuffers);
    dxCtx->DSSetConstantBuffers(0, 4, constantBuffers);
    dxCtx->PSSetConstantBuffers(0, 4, constantBuffers);
    dxCtx->CSSetConstantBuffers(0, 4, constantBuffers);
    ID3D11SamplerState* samplers[] = { ss_Point_, ss_Linear_ };
    dxCtx->VSSetSamplers(0, 2, samplers);
    dxCtx->HSSetSamplers(0, 2, samplers);
    dxCtx->DSSetSamplers(0, 2, samplers);
    dxCtx->PSSetSamplers(0, 2, samplers);
    dxCtx->CSSetSamplers(0, 2, samplers);
    return Status::OK;
}

// NvVolumetricLighting.d3d11.dll: 0x18000A120
Status ContextImp_D3D11::RenderVolume_DoVolume_Directional(RenderVolumeArgs* pArgs)
{
    ID3D11DeviceContext* dxCtx = pArgs->renderCtx;
    ID3D11ShaderResourceView* shadowMap = pArgs->shadowMap;
    const ShadowMapDesc* pShadowMapDesc = pArgs->pShadowMapDesc;
    const VolumeDesc* pVolumeDesc = pArgs->pVolumeDesc;
    uint32_t meshResolution = GetCoarseResolution(pVolumeDesc);

    D3D11_VIEWPORT viewport = { 0.0f, 0.0f, (float)GetInternalViewportWidth(), (float)GetInternalViewportHeight(), 0.0f, 1.0f };
    dxCtx->RSSetViewports(1, &viewport);
    dxCtx->RSSetState(rs_CullNone_);
    dxCtx->OMSetBlendState(bs_Additive_, nullptr, 0xFFFFFFFF);
    ID3D11RenderTargetView* rtv = pAccumulation_->getRTV();
    dxCtx->OMSetRenderTargets(1, &rtv, pDepth_->getDSV());

    RenderVolume_HS_Desc hsDesc;
    hsDesc.MAXTESSFACTOR = static_cast<uint32_t>(pVolumeDesc->eTessQuality);
    RenderVolume_DS_Desc dsDesc;

    // shadow map layout
    ShadowMapLayout layout = pShadowMapDesc->eType;
    if (layout < ShadowMapLayout::SIMPLE)
    {
        return Status::INVALID_PARAMETER;
    }
    if (layout <= ShadowMapLayout::CASCADE_ATLAS)
    {
        hsDesc.SHADOWMAPTYPE = SHADOWMAPTYPE_ATLAS;
        dsDesc.SHADOWMAPTYPE = SHADOWMAPTYPE_ATLAS;
    }
    else if (layout == ShadowMapLayout::CASCADE_ARRAY)
    {
        hsDesc.SHADOWMAPTYPE = SHADOWMAPTYPE_ARRAY;
        dsDesc.SHADOWMAPTYPE = SHADOWMAPTYPE_ARRAY;
    }
    else
    {
        return Status::INVALID_PARAMETER;
    }

    // cascade count (0 elements is only accepted for SIMPLE shadow maps)
    switch (pShadowMapDesc->uElementCount)
    {
    case 0:
        if (pShadowMapDesc->eType != ShadowMapLayout::SIMPLE)
        {
            return Status::INVALID_PARAMETER;
        }
        hsDesc.CASCADECOUNT = 0;
        dsDesc.CASCADECOUNT = 0;
        break;
    case 1:
        hsDesc.CASCADECOUNT = 0;
        dsDesc.CASCADECOUNT = 0;
        break;
    case 2:
        hsDesc.CASCADECOUNT = 1;
        dsDesc.CASCADECOUNT = 1;
        break;
    case 3:
        hsDesc.CASCADECOUNT = 2;
        dsDesc.CASCADECOUNT = 2;
        break;
    case 4:
        hsDesc.CASCADECOUNT = 3;
        dsDesc.CASCADECOUNT = 3;
        break;
    default:
        return Status::INVALID_PARAMETER;
    }
    hsDesc.VOLUMETYPE = VOLUMETYPE_FRUSTUM;
    dsDesc.VOLUMETYPE = VOLUMETYPE_FRUSTUM;

    RenderVolume_PS_Desc psDesc;
    psDesc.SAMPLEMODE = IsInternalMSAA();
    psDesc.LIGHTMODE = LIGHTMODE_DIRECTIONAL;
    psDesc.PASSMODE = PASSMODE_GEOMETRY;
    psDesc.ATTENUATIONMODE = static_cast<uint32_t>(AttenuationMode::NONE);

    dxCtx->HSSetShader(shaders_RenderVolume_HS_[hsDesc], nullptr, 0);
    dxCtx->DSSetShader(shaders_RenderVolume_DS_[dsDesc], nullptr, 0);
    dxCtx->PSSetShader(shaders_RenderVolume_PS_[psDesc], nullptr, 0);

    // t1 = shadow map, t4 = phase LUT
    ID3D11ShaderResourceView* srvs[] = { nullptr, shadowMap, nullptr, nullptr, pPhaseLUT_->getSRV() };
    dxCtx->VSSetShaderResources(0, 5, srvs);
    dxCtx->HSSetShaderResources(0, 5, srvs);
    dxCtx->DSSetShaderResources(0, 5, srvs);
    dxCtx->PSSetShaderResources(0, 5, srvs);

    dxCtx->OMSetDepthStencilState(dss_RenderVolume_, 0xFF);
    DrawFrustumGrid(dxCtx, meshResolution);
    dxCtx->HSSetShader(nullptr, nullptr, 0);
    dxCtx->DSSetShader(nullptr, nullptr, 0);
    DrawFrustumBase(dxCtx, meshResolution);

    if ((debugFlags_ & DEBUG_WIREFRAME) == 0)
    {
        // pixels where the volume extends to the far plane
        psDesc.PASSMODE = PASSMODE_SKY;
        dxCtx->PSSetShader(shaders_RenderVolume_PS_[psDesc], nullptr, 0);
        dxCtx->OMSetDepthStencilState(dss_RenderVolume_Sky_, 0xFF);
        DrawFullscreen(dxCtx);

        // resolve the stencil-marked pixels against the scene depth (t2)
        dxCtx->OMSetDepthStencilState(dss_RenderVolume_Final_, 0xFF);
        dxCtx->OMSetRenderTargets(1, &rtv, pDepth_->getReadOnlyDSV());
        srvs[2] = pDepth_->getSRV();
        dxCtx->PSSetShaderResources(2, 1, &srvs[2]);
        psDesc.PASSMODE = PASSMODE_FINAL;
        dxCtx->PSSetShader(shaders_RenderVolume_PS_[psDesc], nullptr, 0);
        DrawFullscreen(dxCtx);
    }
    return Status::OK;
}

// NvVolumetricLighting.d3d11.dll: 0x18000A8D0
Status ContextImp_D3D11::RenderVolume_DoVolume_Spotlight(RenderVolumeArgs* pArgs)
{
    ID3D11DeviceContext* dxCtx = pArgs->renderCtx;
    ID3D11ShaderResourceView* shadowMap = pArgs->shadowMap;
    const LightDesc* pLightDesc = pArgs->pLightDesc;
    const VolumeDesc* pVolumeDesc = pArgs->pVolumeDesc;
    uint32_t meshResolution = GetCoarseResolution(pVolumeDesc);

    ID3D11ShaderResourceView* srvs[16];
    ID3D11UnorderedAccessView* uavs[16];
    memset(srvs, 0, sizeof(srvs));
    memset(uavs, 0, sizeof(uavs));

    // Light LUTs (two compute passes, ping-ponging between LUT [0] and [1])
    if (pLightDesc->Spotlight.eFalloffMode == SpotlightFalloffMode::NONE)
    {
        ComputeLightLUT_CS_Desc csDesc;
        csDesc.LIGHTMODE = LUTMODE_POINT;
        csDesc.ATTENUATIONMODE = static_cast<uint32_t>(pLightDesc->Spotlight.eAttenuationMode);

        uavs[0] = pLightLUT_P_[0]->getUAV();
        dxCtx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        srvs[4] = pPhaseLUT_->getSRV();
        dxCtx->CSSetShaderResources(0, 6, srvs);
        csDesc.COMPUTEPASS = COMPUTEPASS_SCATTER;
        dxCtx->CSSetShader(shaders_ComputeLightLUT_CS_[csDesc], nullptr, 0);
        dxCtx->Dispatch(8, 64, 1);

        uavs[0] = pLightLUT_P_[1]->getUAV();
        dxCtx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        srvs[5] = pLightLUT_P_[0]->getSRV();
        dxCtx->CSSetShaderResources(0, 6, srvs);
        csDesc.COMPUTEPASS = COMPUTEPASS_SUM;
        dxCtx->CSSetShader(shaders_ComputeLightLUT_CS_[csDesc], nullptr, 0);
        dxCtx->Dispatch(1, 128, 1);

        dxCtx->CSSetShader(nullptr, nullptr, 0);
        uavs[0] = nullptr;
        dxCtx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    }
    else if (pLightDesc->Spotlight.eFalloffMode == SpotlightFalloffMode::FIXED)
    {
        ComputeLightLUT_CS_Desc csDesc;
        csDesc.LIGHTMODE = LUTMODE_SPOTLIGHT;
        csDesc.ATTENUATIONMODE = static_cast<uint32_t>(pLightDesc->Spotlight.eAttenuationMode);

        uavs[0] = pLightLUT_P_[0]->getUAV();
        uavs[1] = pLightLUT_S1_[0]->getUAV();
        uavs[2] = pLightLUT_S2_[0]->getUAV();
        dxCtx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
        srvs[4] = pPhaseLUT_->getSRV();
        dxCtx->CSSetShaderResources(0, 8, srvs);
        csDesc.COMPUTEPASS = COMPUTEPASS_SCATTER;
        dxCtx->CSSetShader(shaders_ComputeLightLUT_CS_[csDesc], nullptr, 0);
        dxCtx->Dispatch(8, 64, 1);

        uavs[0] = pLightLUT_P_[1]->getUAV();
        uavs[1] = pLightLUT_S1_[1]->getUAV();
        uavs[2] = pLightLUT_S2_[1]->getUAV();
        dxCtx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
        srvs[5] = pLightLUT_P_[0]->getSRV();
        srvs[6] = pLightLUT_S1_[0]->getSRV();
        srvs[7] = pLightLUT_S2_[0]->getSRV();
        dxCtx->CSSetShaderResources(0, 8, srvs);
        csDesc.COMPUTEPASS = COMPUTEPASS_SUM;
        dxCtx->CSSetShader(shaders_ComputeLightLUT_CS_[csDesc], nullptr, 0);
        dxCtx->Dispatch(1, 128, 3);

        dxCtx->CSSetShader(nullptr, nullptr, 0);
        memset(uavs, 0, 3 * sizeof(uavs[0]));
        dxCtx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
    }
    // SpotlightFalloffMode::CUSTOM: no LUT

    D3D11_VIEWPORT viewport = { 0.0f, 0.0f, (float)GetInternalViewportWidth(), (float)GetInternalViewportHeight(), 0.0f, 1.0f };
    dxCtx->RSSetViewports(1, &viewport);
    dxCtx->RSSetState(rs_CullNone_);
    dxCtx->OMSetBlendState(bs_Additive_, nullptr, 0xFFFFFFFF);
    ID3D11RenderTargetView* rtv = pAccumulation_->getRTV();
    dxCtx->OMSetRenderTargets(1, &rtv, pDepth_->getDSV());

    RenderVolume_HS_Desc hsDesc;
    hsDesc.SHADOWMAPTYPE = SHADOWMAPTYPE_ATLAS;
    hsDesc.CASCADECOUNT = 0;
    hsDesc.VOLUMETYPE = VOLUMETYPE_FRUSTUM;
    hsDesc.MAXTESSFACTOR = static_cast<uint32_t>(pVolumeDesc->eTessQuality);
    RenderVolume_DS_Desc dsDesc;
    dsDesc.SHADOWMAPTYPE = SHADOWMAPTYPE_ATLAS;
    dsDesc.CASCADECOUNT = 0;
    dsDesc.VOLUMETYPE = VOLUMETYPE_FRUSTUM;
    RenderVolume_PS_Desc psDesc;
    psDesc.SAMPLEMODE = IsInternalMSAA();
    psDesc.LIGHTMODE = LIGHTMODE_SPOTLIGHT;
    psDesc.PASSMODE = PASSMODE_GEOMETRY;
    psDesc.ATTENUATIONMODE = static_cast<uint32_t>(pLightDesc->Spotlight.eAttenuationMode);
    psDesc.FALLOFFMODE = static_cast<uint32_t>(pLightDesc->Spotlight.eFalloffMode);

    dxCtx->HSSetShader(shaders_RenderVolume_HS_[hsDesc], nullptr, 0);
    dxCtx->DSSetShader(shaders_RenderVolume_DS_[dsDesc], nullptr, 0);
    dxCtx->PSSetShader(shaders_RenderVolume_PS_[psDesc], nullptr, 0);

    // t1 = shadow map, t4 = phase LUT, t5..t7 = light LUTs
    srvs[1] = shadowMap;
    srvs[4] = pPhaseLUT_->getSRV();
    srvs[5] = pLightLUT_P_[1]->getSRV();
    srvs[6] = pLightLUT_S1_[1]->getSRV();
    srvs[7] = pLightLUT_S2_[1]->getSRV();
    dxCtx->VSSetShaderResources(0, 8, srvs);
    dxCtx->HSSetShaderResources(0, 8, srvs);
    dxCtx->DSSetShaderResources(0, 8, srvs);
    dxCtx->PSSetShaderResources(0, 8, srvs);

    dxCtx->OMSetDepthStencilState(dss_RenderVolume_, 0xFF);
    DrawFrustumGrid(dxCtx, meshResolution);
    dxCtx->HSSetShader(nullptr, nullptr, 0);
    dxCtx->DSSetShader(nullptr, nullptr, 0);
    dxCtx->RSSetState(rs_CullFront_);
    DrawFrustumCap(dxCtx, meshResolution);

    dxCtx->OMSetDepthStencilState(dss_RenderVolume_Final_, 0xFF);
    dxCtx->OMSetRenderTargets(1, &rtv, pDepth_->getReadOnlyDSV());
    srvs[2] = pDepth_->getSRV();
    dxCtx->PSSetShaderResources(2, 1, &srvs[2]);
    psDesc.PASSMODE = PASSMODE_FINAL;
    dxCtx->PSSetShader(shaders_RenderVolume_PS_[psDesc], nullptr, 0);
    DrawFullscreen(dxCtx);
    return Status::OK;
}

// NvVolumetricLighting.d3d11.dll: 0x18000B700
Status ContextImp_D3D11::RenderVolume_DoVolume_Omni(RenderVolumeArgs* pArgs)
{
    ID3D11DeviceContext* dxCtx = pArgs->renderCtx;
    ID3D11ShaderResourceView* shadowMap = pArgs->shadowMap;
    const LightDesc* pLightDesc = pArgs->pLightDesc;
    const VolumeDesc* pVolumeDesc = pArgs->pVolumeDesc;
    uint32_t meshResolution = GetCoarseResolution(pVolumeDesc);

    ID3D11ShaderResourceView* srvs[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    ID3D11UnorderedAccessView* uav = nullptr;

    // Light LUT (point)
    ComputeLightLUT_CS_Desc csDesc;
    csDesc.LIGHTMODE = LUTMODE_POINT;
    csDesc.ATTENUATIONMODE = static_cast<uint32_t>(pLightDesc->Omni.eAttenuationMode);

    uav = pLightLUT_P_[0]->getUAV();
    dxCtx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    srvs[4] = pPhaseLUT_->getSRV();
    dxCtx->CSSetShaderResources(0, 6, srvs);
    csDesc.COMPUTEPASS = COMPUTEPASS_SCATTER;
    dxCtx->CSSetShader(shaders_ComputeLightLUT_CS_[csDesc], nullptr, 0);
    dxCtx->Dispatch(8, 64, 1);

    uav = pLightLUT_P_[1]->getUAV();
    dxCtx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    srvs[5] = pLightLUT_P_[0]->getSRV();
    dxCtx->CSSetShaderResources(0, 6, srvs);
    csDesc.COMPUTEPASS = COMPUTEPASS_SUM;
    dxCtx->CSSetShader(shaders_ComputeLightLUT_CS_[csDesc], nullptr, 0);
    dxCtx->Dispatch(1, 128, 1);

    dxCtx->CSSetShader(nullptr, nullptr, 0);
    uav = nullptr;
    dxCtx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

    D3D11_VIEWPORT viewport = { 0.0f, 0.0f, (float)GetInternalViewportWidth(), (float)GetInternalViewportHeight(), 0.0f, 1.0f };
    dxCtx->RSSetViewports(1, &viewport);
    dxCtx->RSSetState(rs_CullNone_);
    dxCtx->OMSetBlendState(bs_Additive_, nullptr, 0xFFFFFFFF);
    ID3D11RenderTargetView* rtv = pAccumulation_->getRTV();
    dxCtx->OMSetRenderTargets(1, &rtv, pDepth_->getDSV());

    // dual paraboloid shadow map (2-slice array)
    RenderVolume_HS_Desc hsDesc;
    hsDesc.SHADOWMAPTYPE = SHADOWMAPTYPE_ARRAY;
    hsDesc.CASCADECOUNT = 0;
    hsDesc.VOLUMETYPE = VOLUMETYPE_PARABOLOID;
    hsDesc.MAXTESSFACTOR = static_cast<uint32_t>(pVolumeDesc->eTessQuality);
    RenderVolume_DS_Desc dsDesc;
    dsDesc.SHADOWMAPTYPE = SHADOWMAPTYPE_ARRAY;
    dsDesc.CASCADECOUNT = 0;
    dsDesc.VOLUMETYPE = VOLUMETYPE_PARABOLOID;
    RenderVolume_PS_Desc psDesc;
    psDesc.SAMPLEMODE = IsInternalMSAA();
    psDesc.LIGHTMODE = LIGHTMODE_OMNI;
    psDesc.PASSMODE = PASSMODE_GEOMETRY;
    psDesc.ATTENUATIONMODE = static_cast<uint32_t>(pLightDesc->Omni.eAttenuationMode);

    dxCtx->HSSetShader(shaders_RenderVolume_HS_[hsDesc], nullptr, 0);
    dxCtx->DSSetShader(shaders_RenderVolume_DS_[dsDesc], nullptr, 0);
    dxCtx->PSSetShader(shaders_RenderVolume_PS_[psDesc], nullptr, 0);

    // t1 = shadow map, t4 = phase LUT, t5 = light LUT
    srvs[1] = shadowMap;
    srvs[5] = pLightLUT_P_[1]->getSRV();
    dxCtx->VSSetShaderResources(0, 6, srvs);
    dxCtx->HSSetShaderResources(0, 6, srvs);
    dxCtx->DSSetShaderResources(0, 6, srvs);
    dxCtx->PSSetShaderResources(0, 6, srvs);

    dxCtx->OMSetDepthStencilState(dss_RenderVolume_, 0xFF);
    DrawOmniVolume(dxCtx, meshResolution);
    dxCtx->HSSetShader(nullptr, nullptr, 0);
    dxCtx->DSSetShader(nullptr, nullptr, 0);

    dxCtx->OMSetDepthStencilState(dss_RenderVolume_Final_, 0xFF);
    dxCtx->OMSetRenderTargets(1, &rtv, pDepth_->getReadOnlyDSV());
    srvs[2] = pDepth_->getSRV();
    dxCtx->PSSetShaderResources(2, 1, &srvs[2]);
    psDesc.PASSMODE = PASSMODE_FINAL;
    dxCtx->PSSetShader(shaders_RenderVolume_PS_[psDesc], nullptr, 0);
    DrawFullscreen(dxCtx);
    return Status::OK;
}

// NvVolumetricLighting.d3d11.dll: 0x18000C020
Status ContextImp_D3D11::RenderVolume_End(RenderVolumeArgs* pArgs)
{
    ID3D11DeviceContext* dxCtx = pArgs->renderCtx;
    ID3D11RenderTargetView* nullRTV = nullptr;
    dxCtx->OMSetRenderTargets(1, &nullRTV, nullptr);
    return Status::OK;
}

////////////////////////////////////////////////////////////////////////////////
// EndAccumulation

// NvVolumetricLighting.d3d11.dll: 0x18000C070
Status ContextImp_D3D11::EndAccumulation_Imp(EndAccumulationArgs* pArgs)
{
    ID3D11DeviceContext* dxCtx = pArgs->renderCtx;
    (void)dxCtx;
    return Status::OK;
}

////////////////////////////////////////////////////////////////////////////////
// ApplyLighting

// NvVolumetricLighting.d3d11.dll: 0x18000C0A0
Status ContextImp_D3D11::ApplyLighting_Start(ApplyLightingArgs* pArgs)
{
    ID3D11DeviceContext* dxCtx = pArgs->renderCtx;
    const PostprocessDesc* pPostprocessDesc = pArgs->pPostprocessDesc;

    SetupCB_PerApply(pPostprocessDesc, pPerApplyCB_->Map(dxCtx));
    pPerApplyCB_->Unmap(dxCtx);

    ID3D11Buffer* constantBuffers[] = { pPerContextCB_->getCB(), pPerFrameCB_->getCB(), nullptr, pPerApplyCB_->getCB() };
    dxCtx->VSSetConstantBuffers(0, 4, constantBuffers);
    dxCtx->PSSetConstantBuffers(0, 4, constantBuffers);
    ID3D11SamplerState* samplers[] = { ss_Point_, ss_Linear_ };
    dxCtx->PSSetSamplers(0, 2, samplers);
    dxCtx->OMSetDepthStencilState(dss_NoDepth_, 0xFF);
    dxCtx->OMSetBlendState(bs_NoBlend_, nullptr, 0xFFFFFFFF);
    D3D11_VIEWPORT viewport = { 0.0f, 0.0f, (float)GetInternalViewportWidth(), (float)GetInternalViewportHeight(), 0.0f, 1.0f };
    dxCtx->RSSetViewports(1, &viewport);
    pAccumulatedOutput_ = pAccumulation_;
    return Status::OK;
}

// Resolves the (possibly multisampled) accumulation and depth into single-sampled targets.
// NvVolumetricLighting.d3d11.dll: 0x18000C310
Status ContextImp_D3D11::ApplyLighting_Resolve(ApplyLightingArgs* pArgs)
{
    ID3D11DeviceContext* dxCtx = pArgs->renderCtx;

    Resolve_PS_Desc psDesc;
    psDesc.SAMPLEMODE = IsInternalMSAA();
    dxCtx->PSSetShader(shaders_Resolve_PS_[psDesc], nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { pAccumulation_->getSRV(), pDepth_->getSRV() };
    dxCtx->PSSetShaderResources(0, 2, srvs);
    ID3D11RenderTargetView* rtvs[] = { pResolvedAccumulation_->getRTV(), pResolvedDepth_->getRTV() };
    dxCtx->OMSetRenderTargets(2, rtvs, nullptr);
    DrawFullscreen(dxCtx);
    pAccumulatedOutput_ = pResolvedAccumulation_;
    ID3D11RenderTargetView* nullRTVs[] = { nullptr, nullptr };
    dxCtx->OMSetRenderTargets(2, nullRTVs, nullptr);
    return Status::OK;
}

// Blends the resolved accumulation with the reprojected history.
// NvVolumetricLighting.d3d11.dll: 0x18000C4D0
Status ContextImp_D3D11::ApplyLighting_TemporalFilter(ApplyLightingArgs* pArgs)
{
    ID3D11DeviceContext* dxCtx = pArgs->renderCtx;

    dxCtx->PSSetShader(shaders_TemporalFilter_PS_[0], nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {
        pResolvedAccumulation_->getSRV(),
        pFilteredAccumulation_[lastFrameIndex_]->getSRV(),
        pResolvedDepth_->getSRV(),
        nullptr,
        pFilteredDepth_[lastFrameIndex_]->getSRV(),
    };
    // NOTE: only 4 views are bound, so tLastDepth (t3) is always null in the original; the history
    // depth prepared in srvs[4] is never used. Reproduced as-is.
    dxCtx->PSSetShaderResources(0, 4, srvs);
    ID3D11RenderTargetView* rtvs[] = {
        pFilteredAccumulation_[nextFrameIndex_]->getRTV(),
        pFilteredDepth_[nextFrameIndex_]->getRTV(),
    };
    dxCtx->OMSetRenderTargets(2, rtvs, nullptr);
    DrawFullscreen(dxCtx);
    pAccumulatedOutput_ = pFilteredAccumulation_[nextFrameIndex_];
    ID3D11RenderTargetView* nullRTVs[] = { nullptr, nullptr };
    dxCtx->OMSetRenderTargets(2, nullRTVs, nullptr);
    return Status::OK;
}

// Upsamples the accumulated in-scattering and composites it over the scene target.
// NvVolumetricLighting.d3d11.dll: 0x18000C6F0
Status ContextImp_D3D11::ApplyLighting_Composite(ApplyLightingArgs* pArgs)
{
    ID3D11DeviceContext* dxCtx = pArgs->renderCtx;
    const PostprocessDesc* pPostprocessDesc = pArgs->pPostprocessDesc;
    ID3D11RenderTargetView* sceneTarget = pArgs->sceneTarget;
    ID3D11ShaderResourceView* sceneDepth = pArgs->sceneDepth;

    D3D11_VIEWPORT viewport = { 0.0f, 0.0f, (float)GetOutputViewportWidth(), (float)GetOutputViewportHeight(), 0.0f, 1.0f };
    dxCtx->RSSetViewports(1, &viewport);
    if ((debugFlags_ & DEBUG_NO_BLENDING) != 0)
    {
        dxCtx->OMSetBlendState(bs_Debug_Blend_, nullptr, 0xFFFFFFFF);
    }
    else
    {
        FLOAT blendFactor[] = {
            pPostprocessDesc->fBlendfactor,
            pPostprocessDesc->fBlendfactor,
            pPostprocessDesc->fBlendfactor,
            pPostprocessDesc->fBlendfactor,
        };
        dxCtx->OMSetBlendState(bs_Additive_Modulate_, blendFactor, 0xFFFFFFFF);
    }
    dxCtx->OMSetRenderTargets(1, &sceneTarget, nullptr);

    Apply_PS_Desc psDesc;
    psDesc.SAMPLEMODE = IsOutputMSAA();
    switch (pPostprocessDesc->eUpsampleQuality)
    {
    case UpsampleQuality::BILINEAR:
        psDesc.UPSAMPLEMODE = UPSAMPLEMODE_BILINEAR;
        break;
    case UpsampleQuality::BILATERAL:
        psDesc.UPSAMPLEMODE = UPSAMPLEMODE_BILATERAL;
        break;
    default:
        psDesc.UPSAMPLEMODE = UPSAMPLEMODE_POINT;
        break;
    }
    if (pPostprocessDesc->bDoFog)
    {
        psDesc.FOGMODE = (pPostprocessDesc->bIgnoreSkyFog == true) ? FOGMODE_NOSKY : FOGMODE_FULL;
    }
    else
    {
        psDesc.FOGMODE = FOGMODE_NONE;
    }
    dxCtx->PSSetShader(shaders_Apply_PS_[psDesc], nullptr, 0);

    // t0 = accumulation, t1 = scene depth, t2 = filtered depth (temporal only), t4 = phase LUT
    ID3D11ShaderResourceView* srvs[] = { pAccumulatedOutput_->getSRV(), sceneDepth, nullptr, nullptr, pPhaseLUT_->getSRV() };
    if (pFilteredDepth_[nextFrameIndex_] != nullptr)
    {
        srvs[2] = pFilteredDepth_[nextFrameIndex_]->getSRV();
    }
    dxCtx->PSSetShaderResources(0, 5, srvs);
    DrawFullscreen(dxCtx);
    ID3D11RenderTargetView* nullRTV = nullptr;
    dxCtx->OMSetRenderTargets(1, &nullRTV, nullptr);
    return Status::OK;
}

// NvVolumetricLighting.d3d11.dll: 0x18000CAE0
Status ContextImp_D3D11::ApplyLighting_End(ApplyLightingArgs* pArgs)
{
    ID3D11DeviceContext* dxCtx = pArgs->renderCtx;
    (void)dxCtx;
    return Status::OK;
}

////////////////////////////////////////////////////////////////////////////////
// Draw helpers (no vertex buffers: geometry is generated from SV_VertexID)

// NvVolumetricLighting.d3d11.dll: 0x18000CB10
Status ContextImp_D3D11::DrawFullscreen(ID3D11DeviceContext* dxCtx)
{
    dxCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dxCtx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    dxCtx->IASetInputLayout(nullptr);
    dxCtx->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
    dxCtx->RSSetState(rs_CullNone_);
    dxCtx->VSSetShader(shaders_Quad_VS_[0], nullptr, 0);
    dxCtx->HSSetShader(nullptr, nullptr, 0);
    dxCtx->DSSetShader(nullptr, nullptr, 0);
    dxCtx->Draw(3, 0);
    return Status::OK;
}

// resolution x resolution quad patches covering the light frustum (tessellated by HS/DS).
// NvVolumetricLighting.d3d11.dll: 0x18000CC50
Status ContextImp_D3D11::DrawFrustumGrid(ID3D11DeviceContext* dxCtx, uint32_t resolution)
{
    dxCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
    dxCtx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    dxCtx->IASetInputLayout(nullptr);
    dxCtx->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
    RenderVolume_VS_Desc vsDesc;
    vsDesc.MESHMODE = MESHMODE_FRUSTUM_GRID;
    dxCtx->VSSetShader(shaders_RenderVolume_VS_[vsDesc], nullptr, 0);
    if ((debugFlags_ & DEBUG_WIREFRAME) != 0)
    {
        dxCtx->RSSetState(rs_Wireframe_);
        dxCtx->OMSetBlendState(bs_NoBlend_, nullptr, 0xFFFFFFFF);
        dxCtx->PSSetShader(shaders_Debug_PS_[0], nullptr, 0);
    }
    uint32_t vertexCount = resolution * 4 * resolution;
    dxCtx->Draw(vertexCount, 0);
    return Status::OK;
}

// Two triangles closing the light frustum (directional lights).
// NvVolumetricLighting.d3d11.dll: 0x18000CDF0
Status ContextImp_D3D11::DrawFrustumBase(ID3D11DeviceContext* dxCtx, uint32_t /*resolution*/)
{
    dxCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dxCtx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    dxCtx->IASetInputLayout(nullptr);
    dxCtx->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
    RenderVolume_VS_Desc vsDesc;
    vsDesc.MESHMODE = MESHMODE_FRUSTUM_BASE;
    dxCtx->VSSetShader(shaders_RenderVolume_VS_[vsDesc], nullptr, 0);
    dxCtx->HSSetShader(nullptr, nullptr, 0);
    dxCtx->DSSetShader(nullptr, nullptr, 0);
    if ((debugFlags_ & DEBUG_WIREFRAME) != 0)
    {
        dxCtx->RSSetState(rs_Wireframe_);
        dxCtx->OMSetBlendState(bs_NoBlend_, nullptr, 0xFFFFFFFF);
        dxCtx->PSSetShader(shaders_Debug_PS_[0], nullptr, 0);
    }
    dxCtx->Draw(6, 0);
    return Status::OK;
}

// Cap of the spotlight frustum.
// NvVolumetricLighting.d3d11.dll: 0x18000CFC0
Status ContextImp_D3D11::DrawFrustumCap(ID3D11DeviceContext* dxCtx, uint32_t resolution)
{
    dxCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dxCtx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    dxCtx->IASetInputLayout(nullptr);
    dxCtx->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
    RenderVolume_VS_Desc vsDesc;
    vsDesc.MESHMODE = MESHMODE_FRUSTUM_CAP;
    dxCtx->VSSetShader(shaders_RenderVolume_VS_[vsDesc], nullptr, 0);
    dxCtx->HSSetShader(nullptr, nullptr, 0);
    dxCtx->DSSetShader(nullptr, nullptr, 0);
    if ((debugFlags_ & DEBUG_WIREFRAME) != 0)
    {
        dxCtx->RSSetState(rs_Wireframe_);
        dxCtx->OMSetBlendState(bs_NoBlend_, nullptr, 0xFFFFFFFF);
        dxCtx->PSSetShader(shaders_Debug_PS_[0], nullptr, 0);
    }
    uint32_t vertexCount = 12 * (resolution + 1) + 6;
    dxCtx->Draw(vertexCount, 0);
    return Status::OK;
}

// Six faces of resolution x resolution quad patches around a point light.
// NvVolumetricLighting.d3d11.dll: 0x18000D1A0
Status ContextImp_D3D11::DrawOmniVolume(ID3D11DeviceContext* dxCtx, uint32_t resolution)
{
    dxCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
    dxCtx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    dxCtx->IASetInputLayout(nullptr);
    dxCtx->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
    RenderVolume_VS_Desc vsDesc;
    vsDesc.MESHMODE = MESHMODE_OMNI_VOLUME;
    dxCtx->VSSetShader(shaders_RenderVolume_VS_[vsDesc], nullptr, 0);
    if ((debugFlags_ & DEBUG_WIREFRAME) != 0)
    {
        dxCtx->RSSetState(rs_Wireframe_);
        dxCtx->OMSetBlendState(bs_NoBlend_, nullptr, 0xFFFFFFFF);
        dxCtx->PSSetShader(shaders_Debug_PS_[0], nullptr, 0);
    }
    uint32_t vertexCount = resolution * 24 * resolution;
    dxCtx->Draw(vertexCount, 0);
    return Status::OK;
}

} // namespace VolumetricLighting
} // namespace Nv

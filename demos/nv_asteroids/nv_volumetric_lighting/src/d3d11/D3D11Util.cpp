// D3D11Util.cpp

#include "D3D11Util.h"

namespace Nv
{
namespace VolumetricLighting
{

////////////////////////////////////////////////////////////////////////////////
// RenderTarget

// NvVolumetricLighting.d3d11.dll: 0x18000E970
RenderTarget::RenderTarget(ID3D11Texture2D* resource, ID3D11ShaderResourceView* srv, ID3D11RenderTargetView* rtv,
                           ID3D11UnorderedAccessView* uav)
    : Resource(resource, srv, uav)
{
    rtv_ = rtv;
}

// NvVolumetricLighting.d3d11.dll: 0x18000E9C0
RenderTarget::~RenderTarget()
{
    SAFE_RELEASE(rtv_);
}

// NvVolumetricLighting.d3d11.dll: 0x18000E4B0
RenderTarget* RenderTarget::Create(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t samples,
                                   DXGI_FORMAT format, const char* /*debugName*/)
{
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;

    CD3D11_TEXTURE2D_DESC texDesc;
    texDesc.ArraySize = 1;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (samples == 1)
    {
        texDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    }
    texDesc.CPUAccessFlags = 0;
    texDesc.Format = format;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.MiscFlags = 0;
    texDesc.SampleDesc.Count = (samples > 1) ? samples : 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    device->CreateTexture2D(&texDesc, nullptr, &texture);
    if (texture == nullptr)
    {
        return nullptr;
    }

    CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    srvDesc.Format = texDesc.Format;
    if (samples > 1)
    {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    }
    else
    {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
    }
    device->CreateShaderResourceView(texture, &srvDesc, &srv);
    if (srv == nullptr)
    {
        texture->Release();
        return nullptr;
    }

    CD3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
    rtvDesc.Format = texDesc.Format;
    if (samples > 1)
    {
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
    }
    else
    {
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = 0;
    }
    device->CreateRenderTargetView(texture, &rtvDesc, &rtv);
    if (rtv == nullptr)
    {
        texture->Release();
        srv->Release();
        return nullptr;
    }

    if (samples == 1)
    {
        CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc;
        uavDesc.Format = texDesc.Format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;
        device->CreateUnorderedAccessView(texture, &uavDesc, &uav);
        if (uav == nullptr)
        {
            texture->Release();
            srv->Release();
            rtv->Release();
            return nullptr;
        }
    }

    return NV_NEW RenderTarget(texture, srv, rtv, uav);
}

////////////////////////////////////////////////////////////////////////////////
// DepthTarget

// NvVolumetricLighting.d3d11.dll: 0x18000EF60
DepthTarget::DepthTarget(ID3D11Texture2D* resource, ID3D11ShaderResourceView* srv, ID3D11DepthStencilView* dsv,
                         ID3D11DepthStencilView* readonly_dsv, ID3D11UnorderedAccessView* uav)
    : Resource(resource, srv, uav)
{
    dsv_ = dsv;
    readonly_dsv_ = readonly_dsv;
}

// NvVolumetricLighting.d3d11.dll: 0x18000EFC0
DepthTarget::~DepthTarget()
{
    SAFE_RELEASE(dsv_);
    SAFE_RELEASE(readonly_dsv_);
}

// NvVolumetricLighting.d3d11.dll: 0x18000EA20
DepthTarget* DepthTarget::Create(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t samples,
                                 DXGI_FORMAT format, uint32_t slices, const char* /*debugName*/)
{
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11DepthStencilView* readonly_dsv = nullptr;

    // typeless resource format / depth SRV format for the requested depth format
    DXGI_FORMAT dsvFormat;
    DXGI_FORMAT texFormat;
    DXGI_FORMAT srvFormat;
    switch (format)
    {
    case DXGI_FORMAT_D32_FLOAT:
        texFormat = DXGI_FORMAT_R32_TYPELESS;
        srvFormat = DXGI_FORMAT_R32_FLOAT;
        dsvFormat = DXGI_FORMAT_D32_FLOAT;
        break;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        texFormat = DXGI_FORMAT_R24G8_TYPELESS;
        srvFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        break;
    case DXGI_FORMAT_D16_UNORM:
        texFormat = DXGI_FORMAT_R16_TYPELESS;
        srvFormat = DXGI_FORMAT_R16_UNORM;
        dsvFormat = DXGI_FORMAT_D16_UNORM;
        break;
    default:
        return nullptr;
    }

    CD3D11_TEXTURE2D_DESC texDesc;
    texDesc.ArraySize = slices;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
    texDesc.CPUAccessFlags = 0;
    texDesc.Format = texFormat;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.MiscFlags = 0;
    texDesc.SampleDesc.Count = (samples > 1) ? samples : 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    device->CreateTexture2D(&texDesc, nullptr, &texture);
    if (texture == nullptr)
    {
        return nullptr;
    }

    CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    srvDesc.Format = srvFormat;
    if (slices == 1)
    {
        if (samples > 1)
        {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        }
        else
        {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = 1;
        }
    }
    else if (samples > 1)
    {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
        srvDesc.Texture2DMSArray.FirstArraySlice = 0;
        srvDesc.Texture2DMSArray.ArraySize = slices;
    }
    else
    {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MostDetailedMip = 0;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.ArraySize = slices;
    }
    device->CreateShaderResourceView(texture, &srvDesc, &srv);
    if (srv == nullptr)
    {
        texture->Release();
        return nullptr;
    }

    CD3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc;
    dsvDesc.Flags = 0;
    dsvDesc.Format = dsvFormat;
    if (slices == 1)
    {
        if (samples > 1)
        {
            dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        }
        else
        {
            dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            dsvDesc.Texture2D.MipSlice = 0;
        }
    }
    else if (samples > 1)
    {
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
        dsvDesc.Texture2DMSArray.FirstArraySlice = 0;
        dsvDesc.Texture2DMSArray.ArraySize = slices;
    }
    else
    {
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.MipSlice = 0;
        dsvDesc.Texture2DArray.FirstArraySlice = 0;
        dsvDesc.Texture2DArray.ArraySize = slices;
    }
    device->CreateDepthStencilView(texture, &dsvDesc, &dsv);

    // read-only view: depth (and stencil, when present) bound read-only
    dsvDesc.Flags |= D3D11_DSV_READ_ONLY_DEPTH;
    if (dsvFormat == DXGI_FORMAT_D24_UNORM_S8_UINT)
    {
        dsvDesc.Flags |= D3D11_DSV_READ_ONLY_STENCIL;
    }
    device->CreateDepthStencilView(texture, &dsvDesc, &readonly_dsv);

    if (dsv == nullptr || readonly_dsv == nullptr)
    {
        // as in the original, a successfully created dsv / readonly_dsv is not released on this path
        texture->Release();
        srv->Release();
        return nullptr;
    }

    return NV_NEW DepthTarget(texture, srv, dsv, readonly_dsv, nullptr);
}

} // namespace VolumetricLighting
} // namespace Nv

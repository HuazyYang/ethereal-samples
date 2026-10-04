// D3D11Util.h
//
// D3D11 resource wrappers used by ContextImp_D3D11: dynamic constant buffers, render targets,
// depth targets and the shader table loader.

#pragma once

#include "../Common.h"

#include <d3d11.h>

namespace Nv
{
namespace VolumetricLighting
{

////////////////////////////////////////////////////////////////////////////////
// Dynamic constant buffer holding one T.
// Instantiations (Create / Map / Unmap / dtor / getCB / ctor):
//   ConstantBuffer<PerContextCB>  0x18000DA50 / 0x18000D9B0 / 0x18000D980 / 0x18000DA00 / 0x18000D970 / 0x18000DBE0
//   ConstantBuffer<PerFrameCB>    0x18000D840 / 0x18000D7A0 / 0x18000D770 / 0x18000D7F0 / 0x18000D760 / 0x18000DBC0
//   ConstantBuffer<PerVolumeCB>   0x18000D630 / 0x18000D590 / 0x18000D560 / 0x18000D5E0 / 0x18000D550 / 0x18000DBA0
//   ConstantBuffer<PerApplyCB>    0x18000D420 / 0x18000D380 / 0x18000D350 / 0x18000D3D0 / 0x18000D340 / 0x18000DB80
// (deleting destructors 0x1800095C0 / 0x180009600 / 0x180009640 / 0x180009680)
template <class T>
class ConstantBuffer
{
public:
    static ConstantBuffer<T>* Create(ID3D11Device* device)
    {
        ID3D11Buffer* buffer = nullptr;
        CD3D11_BUFFER_DESC desc;
        desc.ByteWidth = sizeof(T);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags = 0;
        desc.StructureByteStride = 1;
        device->CreateBuffer(&desc, nullptr, &buffer);
        if (buffer == nullptr)
        {
            return nullptr;
        }
        return NV_NEW ConstantBuffer<T>(buffer);
    }

    ~ConstantBuffer()
    {
        SAFE_RELEASE(buffer_);
    }

    T* Map(ID3D11DeviceContext* ctx)
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        ctx->Map(buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        return static_cast<T*>(mapped.pData);
    }

    void Unmap(ID3D11DeviceContext* ctx)
    {
        ctx->Unmap(buffer_, 0);
    }

    ID3D11Buffer* getCB() const
    {
        return buffer_;
    }

private:
    explicit ConstantBuffer(ID3D11Buffer* buffer) : buffer_(buffer) {}

    ID3D11Buffer* buffer_;
};

////////////////////////////////////////////////////////////////////////////////
// Texture with its shader resource / unordered access views (common base of RenderTarget and
// DepthTarget). Base ctor 0x18000E3B0, base release 0x18000E400.
class Resource
{
public:
    ID3D11ShaderResourceView* getSRV() const { return srv_; }     // 0x180005E50
    ID3D11UnorderedAccessView* getUAV() const { return uav_; }    // 0x180005E60

protected:
    Resource(ID3D11Texture2D* resource, ID3D11ShaderResourceView* srv, ID3D11UnorderedAccessView* uav)
        : resource_(resource), srv_(srv), uav_(uav)
    {
    }

    ~Resource()
    {
        SAFE_RELEASE(resource_);
        SAFE_RELEASE(srv_);
        SAFE_RELEASE(uav_);
    }

    ID3D11Texture2D* resource_;     // +0
    ID3D11ShaderResourceView* srv_; // +8
    ID3D11UnorderedAccessView* uav_;// +16
};

// Color render target (32 bytes). Create 0x18000E4B0, ctor 0x18000E970, dtor 0x18000E9C0.
class RenderTarget : public Resource
{
public:
    // A UAV is created only for single-sampled targets. debugName is not used by the D3D11 backend.
    static RenderTarget* Create(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t samples,
                                DXGI_FORMAT format, const char* debugName);
    ~RenderTarget();

    ID3D11RenderTargetView* getRTV() const { return rtv_; }       // 0x180005E70

private:
    RenderTarget(ID3D11Texture2D* resource, ID3D11ShaderResourceView* srv, ID3D11RenderTargetView* rtv,
                 ID3D11UnorderedAccessView* uav);

    ID3D11RenderTargetView* rtv_;   // +24
};

// Depth-stencil target (40 bytes). Create 0x18000EA20, ctor 0x18000EF60, dtor 0x18000EFC0.
class DepthTarget : public Resource
{
public:
    // format must be DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D24_UNORM_S8_UINT or DXGI_FORMAT_D16_UNORM.
    static DepthTarget* Create(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t samples,
                               DXGI_FORMAT format, uint32_t slices, const char* debugName);
    ~DepthTarget();

    ID3D11DepthStencilView* getDSV() const { return dsv_; }                   // 0x180005E80
    ID3D11DepthStencilView* getReadOnlyDSV() const { return readonly_dsv_; }  // 0x180005E90

private:
    DepthTarget(ID3D11Texture2D* resource, ID3D11ShaderResourceView* srv, ID3D11DepthStencilView* dsv,
                ID3D11DepthStencilView* readonly_dsv, ID3D11UnorderedAccessView* uav);

    ID3D11DepthStencilView* dsv_;           // +24
    ID3D11DepthStencilView* readonly_dsv_;  // +32
};

////////////////////////////////////////////////////////////////////////////////
// Shader table loading.
//
// Every permutation table embedded in the DLL is an array of bytecode pointers (nullptr for
// permutations that were not compiled) plus an array of sizes. LoadShaders allocates an array of
// interface pointers and creates every non-null entry.
// Instantiations: pixel 0x18000DC00, compute 0x18000DD10, vertex 0x18000DE20, hull 0x18000DF30,
// domain 0x18000E040. CreateShader overloads: 0x18000E300 (PS), 0x18000E350 (CS), 0x18000E1F0 (VS),
// 0x18000E240 (HS), 0x18000E2A0 (DS).

inline HRESULT CreateShader(ID3D11Device* device, const void* bytecode, uint32_t size, ID3D11PixelShader** out)
{
    return device->CreatePixelShader(bytecode, size, nullptr, out);
}

inline HRESULT CreateShader(ID3D11Device* device, const void* bytecode, uint32_t size, ID3D11ComputeShader** out)
{
    return device->CreateComputeShader(bytecode, size, nullptr, out);
}

inline HRESULT CreateShader(ID3D11Device* device, const void* bytecode, uint32_t size, ID3D11VertexShader** out)
{
    return device->CreateVertexShader(bytecode, size, nullptr, out);
}

inline HRESULT CreateShader(ID3D11Device* device, const void* bytecode, uint32_t size, ID3D11HullShader** out)
{
    return device->CreateHullShader(bytecode, size, nullptr, out);
}

inline HRESULT CreateShader(ID3D11Device* device, const void* bytecode, uint32_t size, ID3D11DomainShader** out)
{
    return device->CreateDomainShader(bytecode, size, nullptr, out);
}

template <class T>
HRESULT LoadShaders(ID3D11Device* device, const void* const* bytecodes, const uint32_t* sizes, uint32_t count,
                    T**& shaders)
{
    HRESULT hr = S_OK;
    shaders = NV_NEW T*[count];
    for (uint32_t i = 0; i < count; ++i)
    {
        const void* bytecode = bytecodes[i];
        if (bytecode != nullptr)
        {
            hr = CreateShader(device, bytecode, sizes[i], &shaders[i]);
            if (FAILED(hr))
            {
                return hr;
            }
        }
        else
        {
            shaders[i] = nullptr;
        }
    }
    return hr;
}

} // namespace VolumetricLighting
} // namespace Nv

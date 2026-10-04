// SmokeTest.cpp
//
// Minimal D3D11 (WARP) smoke test for NvVolumetricLighting.d3d11.dll.
// The DLL is loaded at run time (LoadLibrary + mangled export names), so the same executable can
// drive the original or the rebuilt DLL:
//
//   NvVolumetricLighting.smoketest.exe <path to NvVolumetricLighting.d3d11.dll> [output.ppm]
//
// It renders a small synthetic scene (depth buffer with a floor and a box, shadow maps with an
// occluder) for a directional light, a spotlight (FIXED falloff) and a point light, runs a few frames
// in two configurations (half resolution / no filter, and full resolution / MSAA2 / temporal filter),
// and prints a hash of the composited image so the outputs of both DLLs can be compared.

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>

#include <Nv/VolumetricLighting/NvVolumetricLighting.h>

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <vector>

using namespace DirectX;
namespace VL = Nv::VolumetricLighting;

typedef VL::Status (__cdecl* PFN_OpenLibrary)(nvidia::NvAllocatorCallback*, nvidia::NvAssertHandler*, const VL::VersionDesc&);
typedef VL::Status (__cdecl* PFN_CloseLibrary)();
typedef VL::Status (__cdecl* PFN_CreateContext)(VL::Context&, const VL::PlatformDesc*, const VL::ContextDesc*);
typedef VL::Status (__cdecl* PFN_ReleaseContext)(VL::Context&);
typedef VL::Status (__cdecl* PFN_BeginAccumulation)(VL::Context, VL::BeginAccumulationArgs*);
typedef VL::Status (__cdecl* PFN_RenderVolume)(VL::Context, VL::RenderVolumeArgs*);
typedef VL::Status (__cdecl* PFN_EndAccumulation)(VL::Context, VL::EndAccumulationArgs*);
typedef VL::Status (__cdecl* PFN_ApplyLighting)(VL::Context, VL::ApplyLightingArgs*);

struct Api
{
    PFN_OpenLibrary OpenLibrary;
    PFN_CloseLibrary CloseLibrary;
    PFN_CreateContext CreateContext;
    PFN_ReleaseContext ReleaseContext;
    PFN_BeginAccumulation BeginAccumulation;
    PFN_RenderVolume RenderVolume;
    PFN_EndAccumulation EndAccumulation;
    PFN_ApplyLighting ApplyLighting;
};

static bool LoadApi(HMODULE dll, Api& api)
{
#define LOAD(FIELD, NAME)                                                 \
    api.FIELD = reinterpret_cast<decltype(api.FIELD)>(GetProcAddress(dll, NAME)); \
    if (api.FIELD == nullptr)                                             \
    {                                                                     \
        printf("missing export %s\n", NAME);                              \
        return false;                                                     \
    }
    LOAD(OpenLibrary, "?OpenLibrary@VolumetricLighting@Nv@@YA?AW4Status@12@PEAVNvAllocatorCallback@nvidia@@PEAVNvAssertHandler@5@AEBUVersionDesc@12@@Z");
    LOAD(CloseLibrary, "?CloseLibrary@VolumetricLighting@Nv@@YA?AW4Status@12@XZ");
    LOAD(CreateContext, "?CreateContext@VolumetricLighting@Nv@@YA?AW4Status@12@AEAPEAXPEBUPlatformDesc@12@PEBUContextDesc@12@@Z");
    LOAD(ReleaseContext, "?ReleaseContext@VolumetricLighting@Nv@@YA?AW4Status@12@AEAPEAX@Z");
    LOAD(BeginAccumulation, "?BeginAccumulation@VolumetricLighting@Nv@@YA?AW4Status@12@PEAXPEAUBeginAccumulationArgs@12@@Z");
    LOAD(RenderVolume, "?RenderVolume@VolumetricLighting@Nv@@YA?AW4Status@12@PEAXPEAURenderVolumeArgs@12@@Z");
    LOAD(EndAccumulation, "?EndAccumulation@VolumetricLighting@Nv@@YA?AW4Status@12@PEAXPEAUEndAccumulationArgs@12@@Z");
    LOAD(ApplyLighting, "?ApplyLighting@VolumetricLighting@Nv@@YA?AW4Status@12@PEAXPEAUApplyLightingArgs@12@@Z");
#undef LOAD
    return true;
}

// Counting allocator: verifies that every allocation made through the callback is released.
class CountingAllocator : public nvidia::NvAllocatorCallback
{
public:
    int live = 0;
    int total = 0;
    void* allocate(size_t size, const char*, const char*, int) override
    {
        ++live;
        ++total;
        return malloc(size);
    }
    void deallocate(void* ptr) override
    {
        if (ptr)
        {
            --live;
        }
        free(ptr);
    }
};

static NvcMat44 ToNvc(const XMMATRIX& m)
{
    NvcMat44 r;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(&r), m);
    return r;
}

static NvcVec3 Vec3(float x, float y, float z)
{
    NvcVec3 v = { x, y, z };
    return v;
}

#define CHECK_HR(x)                                             \
    if (FAILED(x))                                              \
    {                                                           \
        printf("%s failed (line %d)\n", #x, __LINE__);          \
        return 1;                                               \
    }

#define CHECK_VL(x)                                             \
    {                                                           \
        VL::Status s__ = (x);                                   \
        if (s__ != VL::Status::OK)                              \
        {                                                       \
            printf("%s returned %d (line %d)\n", #x, (int)s__, __LINE__); \
            return 1;                                           \
        }                                                       \
    }

struct DepthBuffer
{
    ID3D11Texture2D* tex = nullptr;
    ID3D11DepthStencilView* dsv[4] = {};
    ID3D11ShaderResourceView* srv = nullptr;
};

static HRESULT CreateDepth(ID3D11Device* dev, UINT w, UINT h, UINT slices, UINT samples, DepthBuffer& out)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = slices;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = samples;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &out.tex);
    if (FAILED(hr))
        return hr;
    for (UINT s = 0; s < slices; ++s)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        if (samples > 1)
        {
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        }
        else if (slices > 1)
        {
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            dd.Texture2DArray.FirstArraySlice = s;
            dd.Texture2DArray.ArraySize = 1;
        }
        else
        {
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        }
        hr = dev->CreateDepthStencilView(out.tex, &dd, &out.dsv[s]);
        if (FAILED(hr))
            return hr;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    if (samples > 1)
    {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    }
    else if (slices > 1)
    {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize = slices;
    }
    else
    {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
    }
    return dev->CreateShaderResourceView(out.tex, &sd, &out.srv);
}

static void Release(DepthBuffer& d)
{
    for (auto& v : d.dsv)
        if (v)
            v->Release();
    if (d.srv)
        d.srv->Release();
    if (d.tex)
        d.tex->Release();
}

// Depth stamping helper: draws a screen-space rectangle at a constant depth (no pixel shader).
static const char* s_stampVS =
    "cbuffer cb : register(b0) { float4 rect; float depth; };\n"
    "static const float2 k[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1) };\n"
    "float4 main(uint id : SV_VertexID) : SV_Position\n"
    "{\n"
    "    return float4(lerp(rect.xy, rect.zw, k[id]), depth, 1);\n"
    "}\n";

struct DepthStamper
{
    ID3D11VertexShader* vs = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11DepthStencilState* dss = nullptr;
    ID3D11RasterizerState* rs = nullptr;

    bool Init(ID3D11Device* dev)
    {
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        if (FAILED(D3DCompile(s_stampVS, strlen(s_stampVS), "stamp", nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, &errors)))
        {
            printf("stamp VS: %s\n", errors ? (const char*)errors->GetBufferPointer() : "?");
            return false;
        }
        dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs);
        code->Release();
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 32;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&bd, nullptr, &cb);
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dev->CreateDepthStencilState(&dd, &dss);
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        dev->CreateRasterizerState(&rd, &rs);
        return vs && cb && dss && rs;
    }

    void Release()
    {
        if (vs) vs->Release();
        if (cb) cb->Release();
        if (dss) dss->Release();
        if (rs) rs->Release();
    }

    // Clears to 'base' and draws the rectangle (pixel coordinates) at depth 'value'.
    void Stamp(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT w, UINT h, float base, RECT rect, float value)
    {
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, base, 0);
        float data[8] = {
            2.0f * rect.left / w - 1.0f, 1.0f - 2.0f * rect.top / h,
            2.0f * rect.right / w - 1.0f, 1.0f - 2.0f * rect.bottom / h,
            value, 0, 0, 0 };
        ctx->UpdateSubresource(cb, 0, nullptr, data, 0, 0);
        D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rs);
        ID3D11RenderTargetView* nullRTV = nullptr;
        ctx->OMSetRenderTargets(1, &nullRTV, dsv);
        ctx->OMSetDepthStencilState(dss, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &cb);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->PSSetShader(nullptr, nullptr, 0);
        ctx->Draw(6, 0);
        ctx->OMSetRenderTargets(1, &nullRTV, nullptr);
    }
};

static DepthStamper s_stamper;

static void StampDepth(ID3D11DeviceContext* ctx, ID3D11Device* /*dev*/, ID3D11DepthStencilView* dsv, UINT w, UINT h,
                       float base, RECT rect, float value)
{
    s_stamper.Stamp(ctx, dsv, w, h, base, rect, value);
}

struct RunConfig
{
    const char* name;
    UINT width;
    UINT height;
    UINT sceneSamples;
    VL::DownsampleMode downsample;
    VL::MultisampleMode internalMsaa;
    VL::FilterMode filter;
    VL::UpsampleQuality upsample;
    bool fog;
    VL::TessellationQuality tess;
    bool ignoreSkyFog;
    VL::SpotlightFalloffMode spotFalloff;
    VL::AttenuationMode spotAttenuation;
    VL::ShadowMapLayout dirLayout;  // SIMPLE, CASCADE_ATLAS (2x2 atlas) or CASCADE_ARRAY
    uint32_t dirCascades;
    VL::DebugFlags debugFlags;
};

static uint64_t Fnv1a(const uint8_t* p, size_t n, uint64_t h = 1469598103934665603ull)
{
    for (size_t i = 0; i < n; ++i)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static int RunConfiguration(const Api& api, ID3D11Device* dev, ID3D11DeviceContext* ctx, const RunConfig& cfg,
                            const char* ppmPath, uint64_t& hashOut)
{
    const UINT W = cfg.width, H = cfg.height;

    // scene color
    ID3D11Texture2D* colorTex = nullptr;
    ID3D11RenderTargetView* colorRTV = nullptr;
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = W;
        td.Height = H;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = cfg.sceneSamples;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        CHECK_HR(dev->CreateTexture2D(&td, nullptr, &colorTex));
        CHECK_HR(dev->CreateRenderTargetView(colorTex, nullptr, &colorRTV));
    }
    DepthBuffer sceneDepth, shadowDir, shadowSpot, shadowOmni;
    CHECK_HR(CreateDepth(dev, W, H, 1, cfg.sceneSamples, sceneDepth));
    const bool dirArray = (cfg.dirLayout == VL::ShadowMapLayout::CASCADE_ARRAY);
    const bool dirAtlas = (cfg.dirLayout == VL::ShadowMapLayout::CASCADE_ATLAS);
    const UINT dirSize = dirAtlas ? 1024 : 512;
    CHECK_HR(CreateDepth(dev, dirSize, dirSize, dirArray ? cfg.dirCascades : 1, 1, shadowDir));
    CHECK_HR(CreateDepth(dev, 512, 512, 1, 1, shadowSpot));
    CHECK_HR(CreateDepth(dev, 512, 512, 2, 1, shadowOmni));

    // camera
    XMVECTOR eye = XMVectorSet(0.0f, 3.0f, -10.0f, 1.0f);
    XMMATRIX view = XMMatrixLookAtLH(eye, XMVectorSet(0, 1, 0, 1), XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)W / (float)H, 0.5f, 100.0f);
    XMMATRIX viewProj = view * proj;

    // lights
    XMMATRIX dirView = XMMatrixLookAtLH(XMVectorSet(10, 20, -5, 1), XMVectorSet(0, 0, 0, 1), XMVectorSet(0, 1, 0, 0));
    XMMATRIX dirProj = XMMatrixOrthographicLH(30.0f, 30.0f, 1.0f, 60.0f);
    XMMATRIX dirViewProj = dirView * dirProj;
    XMVECTOR spotPos = XMVectorSet(-3, 6, 0, 1);
    XMMATRIX spotView = XMMatrixLookAtLH(spotPos, XMVectorSet(-1, 0, 2, 1), XMVectorSet(0, 1, 0, 0));
    XMMATRIX spotProj = XMMatrixPerspectiveFovLH(XM_PIDIV4 * 1.2f, 1.0f, 0.5f, 20.0f);
    XMMATRIX spotViewProj = spotView * spotProj;
    XMVECTOR omniPos = XMVectorSet(3, 2, 2, 1);
    XMMATRIX omniViewProj = XMMatrixTranslationFromVector(XMVectorNegate(omniPos));

    VL::ContextDesc contextDesc = {};
    contextDesc.framebuffer.uWidth = W;
    contextDesc.framebuffer.uHeight = H;
    contextDesc.framebuffer.uSamples = cfg.sceneSamples;
    contextDesc.eDownsampleMode = cfg.downsample;
    contextDesc.eInternalSampleMode = cfg.internalMsaa;
    contextDesc.eFilterMode = cfg.filter;

    VL::PlatformDesc platformDesc = {};
    platformDesc.platform = VL::PlatformName::D3D11;
    platformDesc.d3d11.pDevice = dev;

    VL::Context vlCtx = nullptr;
    CHECK_VL(api.CreateContext(vlCtx, &platformDesc, &contextDesc));

    const int FRAMES = 3;
    for (int frame = 0; frame < FRAMES; ++frame)
    {
        // synthetic inputs: scene depth with a box, shadow maps with an occluder
        RECT sceneBox = { (LONG)(W / 3), (LONG)(H / 3), (LONG)(W / 2), (LONG)(H * 3 / 4) };
        StampDepth(ctx, dev, sceneDepth.dsv[0], W, H, 0.999f, sceneBox, 0.97f);
        RECT occluder = { 200, 200, 300, 260 };
        for (uint32_t c = 0; c < (dirArray ? cfg.dirCascades : 1); ++c)
        {
            StampDepth(ctx, dev, shadowDir.dsv[c], dirSize, dirSize, 1.0f, occluder, 0.4f);
        }
        StampDepth(ctx, dev, shadowSpot.dsv[0], 512, 512, 1.0f, occluder, 0.6f);
        StampDepth(ctx, dev, shadowOmni.dsv[0], 512, 512, 1.0f, occluder, 0.5f);
        StampDepth(ctx, dev, shadowOmni.dsv[1], 512, 512, 1.0f, occluder, 0.5f);
        FLOAT sky[4] = { 0.1f, 0.15f, 0.25f, 1.0f };
        ctx->ClearRenderTargetView(colorRTV, sky);

        VL::ViewerDesc viewerDesc = {};
        viewerDesc.mProj = ToNvc(proj);
        viewerDesc.mViewProj = ToNvc(viewProj);
        XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&viewerDesc.vEyePosition), eye);
        viewerDesc.uViewportWidth = W;
        viewerDesc.uViewportHeight = H;

        VL::MediumDesc mediumDesc = {};
        mediumDesc.vAbsorption = Vec3(0.002f, 0.002f, 0.002f);
        mediumDesc.uNumPhaseTerms = 2;
        mediumDesc.PhaseTerms[0].ePhaseFunc = VL::PhaseFunctionType::HENYEYGREENSTEIN;
        mediumDesc.PhaseTerms[0].vDensity = Vec3(0.02f, 0.02f, 0.02f);
        mediumDesc.PhaseTerms[0].fEccentricity = 0.6f;
        mediumDesc.PhaseTerms[1].ePhaseFunc = VL::PhaseFunctionType::RAYLEIGH;
        mediumDesc.PhaseTerms[1].vDensity = Vec3(0.005f, 0.01f, 0.02f);

        VL::BeginAccumulationArgs beginArgs = {};
        beginArgs.renderCtx = ctx;
        beginArgs.pViewerDesc = &viewerDesc;
        beginArgs.pMediumDesc = &mediumDesc;
        beginArgs.debugFlags = cfg.debugFlags;
        beginArgs.sceneDepth = sceneDepth.srv;
        CHECK_VL(api.BeginAccumulation(vlCtx, &beginArgs));

        VL::VolumeDesc volumeDesc = {};
        volumeDesc.fTargetRayResolution = 12.0f;
        volumeDesc.uMaxMeshResolution = 256;
        volumeDesc.fDepthBias = 0.0f;
        volumeDesc.eTessQuality = cfg.tess;

        // directional light
        {
            VL::ShadowMapDesc sm = {};
            sm.eType = cfg.dirLayout;
            sm.uWidth = dirSize;
            sm.uHeight = dirSize;
            sm.uElementCount = cfg.dirCascades;
            for (uint32_t c = 0; c < cfg.dirCascades; ++c)
            {
                // cascade c covers a (c+1) times larger area
                XMMATRIX cascadeProj = XMMatrixOrthographicLH(15.0f * (c + 1), 15.0f * (c + 1), 1.0f, 60.0f);
                sm.Elements[c].mViewProj = ToNvc((cfg.dirCascades > 1) ? dirView * cascadeProj : dirViewProj);
                sm.Elements[c].uOffsetX = dirAtlas ? (c % 2) * 512 : 0;
                sm.Elements[c].uOffsetY = dirAtlas ? (c / 2) * 512 : 0;
                sm.Elements[c].uWidth = 512;
                sm.Elements[c].uHeight = 512;
                sm.Elements[c].mArrayIndex = dirArray ? c : 0;
            }
            VL::LightDesc light = {};
            light.eType = VL::LightType::DIRECTIONAL;
            light.mLightToWorld = ToNvc(XMMatrixInverse(nullptr, dirViewProj));
            light.vIntensity = Vec3(20.0f, 18.0f, 15.0f);
            light.Directional.vDirection = Vec3(-0.4f, -0.8f, 0.2f);
            VL::RenderVolumeArgs args = {};
            args.renderCtx = ctx;
            args.pShadowMapDesc = &sm;
            args.pLightDesc = &light;
            args.pVolumeDesc = &volumeDesc;
            args.shadowMap = shadowDir.srv;
            CHECK_VL(api.RenderVolume(vlCtx, &args));
        }
        // spotlight, FIXED falloff, polynomial attenuation
        {
            VL::ShadowMapDesc sm = {};
            sm.eType = VL::ShadowMapLayout::SIMPLE;
            sm.uWidth = 512;
            sm.uHeight = 512;
            sm.uElementCount = 1;
            sm.Elements[0].mViewProj = ToNvc(spotViewProj);
            sm.Elements[0].uWidth = 512;
            sm.Elements[0].uHeight = 512;
            VL::LightDesc light = {};
            light.eType = VL::LightType::SPOTLIGHT;
            light.mLightToWorld = ToNvc(XMMatrixInverse(nullptr, spotViewProj));
            light.vIntensity = Vec3(200.0f, 150.0f, 100.0f);
            light.Spotlight.vDirection = Vec3(0.25f, -0.75f, 0.25f);
            XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&light.Spotlight.vPosition), spotPos);
            light.Spotlight.fZNear = 0.5f;
            light.Spotlight.fZFar = 20.0f;
            light.Spotlight.eFalloffMode = cfg.spotFalloff;
            light.Spotlight.fFalloff_CosTheta = cosf(XM_PIDIV4 * 0.6f);
            light.Spotlight.fFalloff_Power = 1.0f;
            light.Spotlight.eAttenuationMode = cfg.spotAttenuation;
            light.Spotlight.fAttenuationFactors[0] = 0.0f;
            light.Spotlight.fAttenuationFactors[1] = 0.05f;
            light.Spotlight.fAttenuationFactors[2] = 0.0f;
            light.Spotlight.fAttenuationFactors[3] = 0.0f;
            VL::RenderVolumeArgs args = {};
            args.renderCtx = ctx;
            args.pShadowMapDesc = &sm;
            args.pLightDesc = &light;
            args.pVolumeDesc = &volumeDesc;
            args.shadowMap = shadowSpot.srv;
            CHECK_VL(api.RenderVolume(vlCtx, &args));
        }
        // point light, dual paraboloid shadow map
        {
            VL::ShadowMapDesc sm = {};
            sm.eType = VL::ShadowMapLayout::PARABOLOID;
            sm.uWidth = 512;
            sm.uHeight = 512;
            sm.uElementCount = 2;
            for (int i = 0; i < 2; ++i)
            {
                sm.Elements[i].mViewProj = ToNvc(omniViewProj);
                sm.Elements[i].uWidth = 512;
                sm.Elements[i].uHeight = 512;
                sm.Elements[i].mArrayIndex = i;
            }
            VL::LightDesc light = {};
            light.eType = VL::LightType::POINT;
            light.mLightToWorld = ToNvc(XMMatrixInverse(nullptr, omniViewProj));
            light.vIntensity = Vec3(50.0f, 80.0f, 120.0f);
            XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&light.Omni.vPosition), omniPos);
            light.Omni.fZNear = 0.5f;
            light.Omni.fZFar = 10.0f;
            light.Omni.eAttenuationMode = VL::AttenuationMode::INV_POLYNOMIAL;
            light.Omni.fAttenuationFactors[0] = 1.0f;
            light.Omni.fAttenuationFactors[1] = 0.5f;
            light.Omni.fAttenuationFactors[2] = 0.1f;
            light.Omni.fAttenuationFactors[3] = 0.0f;
            VL::RenderVolumeArgs args = {};
            args.renderCtx = ctx;
            args.pShadowMapDesc = &sm;
            args.pLightDesc = &light;
            args.pVolumeDesc = &volumeDesc;
            args.shadowMap = shadowOmni.srv;
            CHECK_VL(api.RenderVolume(vlCtx, &args));
        }

        VL::EndAccumulationArgs endArgs = {};
        endArgs.renderCtx = ctx;
        CHECK_VL(api.EndAccumulation(vlCtx, &endArgs));

        VL::PostprocessDesc post = {};
        post.mUnjitteredViewProj = ToNvc(viewProj);
        post.fTemporalFactor = 0.9f;
        post.fFilterThreshold = 0.2f;
        post.eUpsampleQuality = cfg.upsample;
        post.vFogLight = Vec3(1.0f, 1.0f, 1.0f);
        post.fMultiscatter = 0.5f;
        post.bDoFog = cfg.fog;
        post.bIgnoreSkyFog = cfg.ignoreSkyFog;
        post.fBlendfactor = 1.0f;
        VL::ApplyLightingArgs applyArgs = {};
        applyArgs.renderCtx = ctx;
        applyArgs.pPostprocessDesc = &post;
        applyArgs.sceneTarget = colorRTV;
        applyArgs.sceneDepth = sceneDepth.srv;
        CHECK_VL(api.ApplyLighting(vlCtx, &applyArgs));
    }

    CHECK_VL(api.ReleaseContext(vlCtx));

    // read back (resolve first if multisampled)
    ID3D11Texture2D* resolved = colorTex;
    resolved->AddRef();
    if (cfg.sceneSamples > 1)
    {
        resolved->Release();
        D3D11_TEXTURE2D_DESC td;
        colorTex->GetDesc(&td);
        td.SampleDesc.Count = 1;
        td.BindFlags = 0;
        CHECK_HR(dev->CreateTexture2D(&td, nullptr, &resolved));
        ctx->ResolveSubresource(resolved, 0, colorTex, 0, td.Format);
    }
    D3D11_TEXTURE2D_DESC td;
    resolved->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.SampleDesc.Count = 1;
    ID3D11Texture2D* staging = nullptr;
    CHECK_HR(dev->CreateTexture2D(&td, nullptr, &staging));
    ctx->CopyResource(staging, resolved);
    D3D11_MAPPED_SUBRESOURCE mapped;
    CHECK_HR(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
    std::vector<uint16_t> pixels(W * H * 4);
    for (UINT y = 0; y < H; ++y)
    {
        memcpy(&pixels[y * W * 4], (const uint8_t*)mapped.pData + y * mapped.RowPitch, W * 8);
    }
    ctx->Unmap(staging, 0);
    hashOut = Fnv1a(reinterpret_cast<const uint8_t*>(pixels.data()), pixels.size() * 2);

    double sum[3] = {};
    std::vector<uint8_t> rgb(W * H * 3);
    for (UINT i = 0; i < W * H; ++i)
    {
        for (int c = 0; c < 3; ++c)
        {
            float f = PackedVector::XMConvertHalfToFloat(pixels[i * 4 + c]);
            sum[c] += f;
            float t = f / (1.0f + f);   // simple tonemap for the preview
            rgb[i * 3 + c] = (uint8_t)(t * 255.0f + 0.5f);
        }
    }
    printf("[%s] hash %016llx  mean rgb (%.5f, %.5f, %.5f)\n", cfg.name, (unsigned long long)hashOut,
           sum[0] / (W * H), sum[1] / (W * H), sum[2] / (W * H));
    if (ppmPath)
    {
        FILE* f = fopen(ppmPath, "wb");
        if (f)
        {
            fprintf(f, "P6\n%u %u\n255\n", W, H);
            fwrite(rgb.data(), 1, rgb.size(), f);
            fclose(f);
        }
    }

    staging->Release();
    resolved->Release();
    Release(sceneDepth);
    Release(shadowDir);
    Release(shadowSpot);
    Release(shadowOmni);
    colorRTV->Release();
    colorTex->Release();
    return 0;
}

int main(int argc, char** argv)
{
    const char* dllPath = (argc > 1) ? argv[1] : "NvVolumetricLighting.d3d11.dll";
    const char* ppmPrefix = (argc > 2) ? argv[2] : nullptr;

    HMODULE dll = LoadLibraryA(dllPath);
    if (dll == nullptr)
    {
        printf("cannot load %s (error %lu)\n", dllPath, GetLastError());
        return 1;
    }
    Api api;
    if (!LoadApi(dll, api))
    {
        return 1;
    }

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    CHECK_HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx));

    if (!s_stamper.Init(dev))
    {
        return 1;
    }

    CountingAllocator allocator;
    CHECK_VL(api.OpenLibrary(&allocator, nullptr, VL::VersionDesc()));

    RunConfig configs[] = {
        { "half-res/none/bilinear/spot-fixed", 320, 240, 1, VL::DownsampleMode::HALF, VL::MultisampleMode::SINGLE,
          VL::FilterMode::NONE, VL::UpsampleQuality::BILINEAR, false, VL::TessellationQuality::HIGH,
          true, VL::SpotlightFalloffMode::FIXED, VL::AttenuationMode::POLYNOMIAL,
          VL::ShadowMapLayout::SIMPLE, 1, VL::DebugFlags::NONE },
        { "full-res/msaa2/temporal/bilateral/fog-nosky/spot-none/atlas3", 256, 192, 2, VL::DownsampleMode::FULL,
          VL::MultisampleMode::MSAA2, VL::FilterMode::TEMPORAL, VL::UpsampleQuality::BILATERAL, true,
          VL::TessellationQuality::LOW, true, VL::SpotlightFalloffMode::NONE, VL::AttenuationMode::INV_POLYNOMIAL,
          VL::ShadowMapLayout::CASCADE_ATLAS, 3, VL::DebugFlags::NONE },
        { "quarter-res/msaa4/point/fog/spot-custom/array4/no-blending", 320, 256, 1, VL::DownsampleMode::QUARTER,
          VL::MultisampleMode::MSAA4, VL::FilterMode::NONE, VL::UpsampleQuality::POINT, true,
          VL::TessellationQuality::MEDIUM, false, VL::SpotlightFalloffMode::CUSTOM, VL::AttenuationMode::POLYNOMIAL,
          VL::ShadowMapLayout::CASCADE_ARRAY, 4, VL::DebugFlags::NO_BLENDING },
        { "half-res/temporal/wireframe/spot-custom-inv", 256, 256, 4, VL::DownsampleMode::HALF,
          VL::MultisampleMode::SINGLE, VL::FilterMode::TEMPORAL, VL::UpsampleQuality::BILATERAL, false,
          VL::TessellationQuality::HIGH, false, VL::SpotlightFalloffMode::CUSTOM, VL::AttenuationMode::INV_POLYNOMIAL,
          VL::ShadowMapLayout::CASCADE_ARRAY, 2, VL::DebugFlags::WIREFRAME },
    };
    uint64_t combined = 1469598103934665603ull;
    int index = 0;
    for (const RunConfig& cfg : configs)
    {
        char path[MAX_PATH];
        if (ppmPrefix)
        {
            sprintf_s(path, "%s_%d.ppm", ppmPrefix, index);
        }
        uint64_t hash = 0;
        if (RunConfiguration(api, dev, ctx, cfg, ppmPrefix ? path : nullptr, hash) != 0)
        {
            return 1;
        }
        combined = Fnv1a(reinterpret_cast<const uint8_t*>(&hash), sizeof(hash), combined);
        ++index;
    }

    CHECK_VL(api.CloseLibrary());
    printf("allocations: %d total, %d still live after ReleaseContext\n", allocator.total, allocator.live);
    printf("combined hash %016llx\n", (unsigned long long)combined);

    s_stamper.Release();
    ctx->Release();
    dev->Release();
    FreeLibrary(dll);
    return 0;
}

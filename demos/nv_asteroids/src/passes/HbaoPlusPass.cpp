#include "passes/HbaoPlusPass.h"
#include "app/GpuProfiler.h"

#include "app/UIData.h"

#include <donut/core/log.h>
#include <donut/engine/View.h>

#include <nvrhi/d3d12.h>

#include <directx/d3d12.h>
#include <GFSDK_SSAO.h>

using namespace donut;
using namespace donut::math;

namespace
{
    // Header version the 2018 executable was built against (passed explicitly: the bundled header says
    // revision 23827312, the shipped DLL only accepts 24284062, the immediate found in Asteroids.exe;
    // 24283550 is rejected with GFSDK_SSAO_VERSION_MISMATCH).
    GFSDK_SSAO_Version GetHbaoVersion2018()
    {
        GFSDK_SSAO_Version version;
        version.Major = 4;
        version.Minor = 0;
        version.Branch = 0;
        version.Revision = 24284062;
        return version;
    }

    nvrhi::d3d12::IDevice* GetD3D12Device(nvrhi::IDevice* device)
    {
        // deviation: with the nvrhi validation layer the device is a wrapper without access to the D3D12
        // descriptor heaps; HBAO+ is then disabled.
        //
        // QueryInterface replaces dynamic_cast (nvrhi ADR 0006), which throws on the ABI-stable
        // interfaces. The validation wrapper does not answer for d3d12::IDevice, so HBAO+ is still
        // disabled there; note that getNativeObject(Nvrhi_D3D12_Device) would not do, because the
        // wrapper forwards it to the real device. The reference is released right away: the caller
        // only borrows the pointer, and m_Device owns the device for the lifetime of the pass.
        nvrhi::AutoPtr<nvrhi::d3d12::IDevice> d3d12Device;
        if (NVRHI_FAILED(device->QueryInterface(NVRHI_IID_PPV_ARGS(&d3d12Device))))
            return nullptr;
        return d3d12Device.Get();
    }
}

HbaoPlusPass::HbaoPlusPass(nvrhi::IDevice* device)
    : m_Device(device)
{
    CreateContext();
}

HbaoPlusPass::~HbaoPlusPass()
{
    ReleaseContext();
}

void* HbaoPlusPass::GetCurrentSrvHeap() const
{
    nvrhi::d3d12::IDevice* d3d12Device = GetD3D12Device(m_Device);
    if (!d3d12Device)
        return nullptr;
    return d3d12Device->getDescriptorHeap(nvrhi::d3d12::DescriptorHeapType::ShaderResourceView)->getShaderVisibleHeap();
}

void* HbaoPlusPass::GetCurrentRtvHeap() const
{
    nvrhi::d3d12::IDevice* d3d12Device = GetD3D12Device(m_Device);
    if (!d3d12Device)
        return nullptr;
    return d3d12Device->getDescriptorHeap(nvrhi::d3d12::DescriptorHeapType::RenderTargetView)->getHeap();
}

void HbaoPlusPass::CreateContext()
{
    nvrhi::d3d12::IDevice* d3d12Device = GetD3D12Device(m_Device);
    if (!d3d12Device)
    {
        log::warning("HBAO+ is not available with the nvrhi validation layer");
        return;
    }

    nvrhi::d3d12::IDescriptorHeap* srvHeap = d3d12Device->getDescriptorHeap(nvrhi::d3d12::DescriptorHeapType::ShaderResourceView);
    nvrhi::d3d12::IDescriptorHeap* rtvHeap = d3d12Device->getDescriptorHeap(nvrhi::d3d12::DescriptorHeapType::RenderTargetView);

    m_SrvBaseIndex = srvHeap->allocateDescriptors(GFSDK_SSAO_NUM_DESCRIPTORS_CBV_SRV_UAV_HEAP_D3D12);
    m_SrvHeap = srvHeap->getShaderVisibleHeap();
    m_RtvBaseIndex = rtvHeap->allocateDescriptors(GFSDK_SSAO_NUM_DESCRIPTORS_RTV_HEAP_D3D12);
    m_RtvHeap = rtvHeap->getHeap();
    m_HasRanges = true;

    GFSDK_SSAO_DescriptorHeaps_D3D12 heaps;
    heaps.CBV_SRV_UAV.pDescHeap = static_cast<ID3D12DescriptorHeap*>(m_SrvHeap);
    heaps.CBV_SRV_UAV.BaseIndex = m_SrvBaseIndex;
    heaps.RTV.pDescHeap = static_cast<ID3D12DescriptorHeap*>(m_RtvHeap);
    heaps.RTV.BaseIndex = m_RtvBaseIndex;

    ID3D12Device* nativeDevice = static_cast<ID3D12Device*>(
        m_Device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device));
    const GFSDK_SSAO_Status status = GFSDK_SSAO_CreateContext_D3D12(nativeDevice, 1, heaps, &m_Context, nullptr,
        GetHbaoVersion2018());
    if (status != GFSDK_SSAO_OK)
    {
        log::warning("GFSDK_SSAO_CreateContext_D3D12 failed (status %d); HBAO+ is disabled", int(status));
        m_Context = nullptr;
    }
}

void HbaoPlusPass::ReleaseContext()
{
    if (m_Context)
    {
        m_Context->Release();
        m_Context = nullptr;
    }

    if (!m_HasRanges)
        return;

    if (nvrhi::d3d12::IDevice* d3d12Device = GetD3D12Device(m_Device))
    {
        d3d12Device->getDescriptorHeap(nvrhi::d3d12::DescriptorHeapType::ShaderResourceView)
            ->releaseDescriptors(m_SrvBaseIndex, GFSDK_SSAO_NUM_DESCRIPTORS_CBV_SRV_UAV_HEAP_D3D12);
        d3d12Device->getDescriptorHeap(nvrhi::d3d12::DescriptorHeapType::RenderTargetView)
            ->releaseDescriptors(m_RtvBaseIndex, GFSDK_SSAO_NUM_DESCRIPTORS_RTV_HEAP_D3D12);
    }
    m_SrvHeap = nullptr;
    m_RtvHeap = nullptr;
    m_HasRanges = false;
}

void HbaoPlusPass::Render(nvrhi::ICommandList* commandList, const HbaoParameters& params,
    const engine::ICompositeView& compositeView, nvrhi::ITexture* depth, nvrhi::ITexture* normals, nvrhi::ITexture* output)
{
    demo::ProfBegin(commandList, "HBAO+");

    // nvrhi grows its heaps on demand; the descriptor ranges then live in a dead heap.
    if (m_HasRanges && (GetCurrentSrvHeap() != m_SrvHeap || GetCurrentRtvHeap() != m_RtvHeap))
    {
        ReleaseContext();
        CreateContext();
    }

    if (!m_Context || !depth || !output)
    {
        demo::ProfEnd(commandList);
        return;
    }

    GFSDK_SSAO_Parameters aoParams;
    aoParams.Radius = params.radiusWorld;
    aoParams.Bias = params.surfaceBias;
    aoParams.SmallScaleAO = params.amount * 0.5f;
    aoParams.LargeScaleAO = params.amount * 0.5f;
    aoParams.PowerExponent = params.powerExponent;
    aoParams.ForegroundAO.Enable = false;
    aoParams.BackgroundAO.Enable = true;
    aoParams.BackgroundAO.BackgroundViewDepth = params.backgroundViewDepth;
    aoParams.StepCount = GFSDK_SSAO_STEP_COUNT_4;
    aoParams.DepthStorage = GFSDK_SSAO_FP32_VIEW_DEPTHS;
    aoParams.DepthClampMode = GFSDK_SSAO_CLAMP_TO_EDGE;
    aoParams.Blur.Enable = true;
    aoParams.Blur.Radius = GFSDK_SSAO_BLUR_RADIUS_4;
    aoParams.Blur.Sharpness = 16.f;

    // HBAO+ reads the textures through native descriptors and records native commands.
    commandList->setTextureState(depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    if (normals)
        commandList->setTextureState(normals, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(output, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    commandList->commitBarriers();

    ID3D12GraphicsCommandList* nativeCommandList = static_cast<ID3D12GraphicsCommandList*>(
        commandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList));
    ID3D12CommandQueue* nativeQueue = static_cast<ID3D12CommandQueue*>(
        m_Device->getNativeQueue(nvrhi::ObjectTypes::D3D12_CommandQueue, nvrhi::CommandQueue::Graphics));

    for (uint32_t viewIndex = 0; viewIndex < compositeView.GetNumChildViews(engine::ViewType::PLANAR); viewIndex++)
    {
        const engine::IView* view = compositeView.GetChildView(engine::ViewType::PLANAR, viewIndex);

        // World-to-view (translated world) as a row-major 4x4.
        float4x4 worldToView = affineToHomogeneous(view->GetViewMatrix());
        float4x4 projection = view->GetProjectionMatrix(false);

        // The pipeline is left-handed and carries the 2018 content's handedness in the projection
        // (P[0][0] < 0, see app/RenderHandedness.h). HBAO+ accepts such a projection without an error
        // but reconstructs view-space positions as if its focal terms were positive, which no longer
        // agrees with the normals it rotates by worldToView. Hand it the same world-to-clip transform
        // with the reflection moved from the projection onto worldToView: the occlusion is invariant
        // under a reflection of view space, so the result is the 2018 one.
        if (projection[0][0] < 0.f)
        {
            const float4x4 mirrorX = affineToHomogeneous(scaling(float3(-1.f, 1.f, 1.f)));
            worldToView = worldToView * mirrorX;
            projection = mirrorX * projection;
        }

        // HBAO+ wants the projection normalized so that m[2][3] = 1.
        projection *= 1.f / projection[2][3];
        projection[2][3] = 1.f;

        const nvrhi::ViewportState viewportState = view->GetViewportState();
        const nvrhi::Viewport& viewport = viewportState.viewports[0];
        const nvrhi::TextureSubresourceSet subresources = view->GetSubresources();

        GFSDK_SSAO_InputData_D3D12 input;
        input.DepthData.DepthTextureType = GFSDK_SSAO_HARDWARE_DEPTHS;
        memcpy(&input.DepthData.ProjectionMatrix.Data, &projection, sizeof(float4x4));
        input.DepthData.ProjectionMatrix.Layout = GFSDK_SSAO_ROW_MAJOR_ORDER;
        input.DepthData.MetersToViewSpaceUnits = 1.f;
        input.DepthData.Viewport.Enable = true;
        input.DepthData.Viewport.TopLeftX = uint32_t(viewport.minX);
        input.DepthData.Viewport.TopLeftY = uint32_t(viewport.minY);
        input.DepthData.Viewport.Width = uint32_t(viewport.maxX - viewport.minX);
        input.DepthData.Viewport.Height = uint32_t(viewport.maxY - viewport.minY);
        input.DepthData.Viewport.MinDepth = viewport.minZ;
        input.DepthData.Viewport.MaxDepth = viewport.maxZ;
        input.DepthData.FullResDepthTextureSRV.pResource = static_cast<ID3D12Resource*>(
            depth->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource));
        input.DepthData.FullResDepthTextureSRV.GpuHandle = reinterpret_cast<uint64_t>(depth->getNativeView(
            nvrhi::ObjectTypes::D3D12_ShaderResourceViewGpuDescriptor, nvrhi::Format::UNKNOWN, subresources));

        if (normals)
        {
            input.NormalData.Enable = true;
            memcpy(&input.NormalData.WorldToViewMatrix.Data, &worldToView, sizeof(float4x4));
            input.NormalData.WorldToViewMatrix.Layout = GFSDK_SSAO_ROW_MAJOR_ORDER;
            input.NormalData.DecodeScale = 1.f;
            input.NormalData.DecodeBias = 0.f;
            input.NormalData.FullResNormalTextureSRV.pResource = static_cast<ID3D12Resource*>(
                normals->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource));
            input.NormalData.FullResNormalTextureSRV.GpuHandle = reinterpret_cast<uint64_t>(normals->getNativeView(
                nvrhi::ObjectTypes::D3D12_ShaderResourceViewGpuDescriptor, nvrhi::Format::UNKNOWN, subresources));
        }

        GFSDK_SSAO_RenderTargetView_D3D12 outputView;
        outputView.pResource = static_cast<ID3D12Resource*>(
            output->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource));
        outputView.CpuHandle = reinterpret_cast<size_t>(output->getNativeView(
            nvrhi::ObjectTypes::D3D12_RenderTargetViewDescriptor, nvrhi::Format::UNKNOWN, subresources));

        GFSDK_SSAO_Output_D3D12 aoOutput;
        aoOutput.pRenderTargetView = &outputView;
        aoOutput.Blend.Mode = GFSDK_SSAO_MULTIPLY_RGB;

        // The 2018 code skipped the draw when nvrhi had swapped a heap between the checks above and here.
        if (GetCurrentSrvHeap() == m_SrvHeap && GetCurrentRtvHeap() == m_RtvHeap)
        {
            const GFSDK_SSAO_Status status = m_Context->RenderAO(nativeQueue, nativeCommandList, input, aoParams,
                aoOutput, GFSDK_SSAO_RENDER_AO);
            if (status != GFSDK_SSAO_OK)
            {
                static bool s_Reported = false;
                if (!s_Reported)
                    log::warning("HBAO+ RenderAO failed (status %d)", int(status));
                s_Reported = true;
            }
        }
    }

    // HBAO+ changed the descriptor heaps, root signature and pipeline behind nvrhi's back.
    commandList->clearState();
    demo::ProfEnd(commandList);
}

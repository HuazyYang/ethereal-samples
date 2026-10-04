#include "fx/RealStarField.h"

#include "app/RenderHandedness.h"
#include "app/GpuProfiler.h"
#include "fx/FxCommon.h"

#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/View.h>

#include <cmath>
#include <cstddef>
#include <cstring>

using namespace donut::math;
#include <space_cb.h>

using namespace donut::engine;

namespace fx
{
    // ---------------------------------------------------------------------------------------------
    // RealStarFieldResources
    // ---------------------------------------------------------------------------------------------

    RealStarFieldResources::RealStarFieldResources(
        nvrhi::IDevice* device,
        const nvrhi::AutoPtr<donut::vfs::IFileSystem>& fs,
        const std::filesystem::path& mediaPath)
    {
        LoadStars(fs, mediaPath / "SkyAndStars/Stars.buf");

        nvrhi::BufferDesc desc;
        desc.byteSize = uint64_t(m_Stars.size()) * sizeof(StarInstance);
        desc.structStride = sizeof(StarInstance);
        desc.debugName = "Star instance buffer";
        // deviation: the 2018 code tracked the state manually (beginTrackingBufferState(Common) before the
        // upload, setPermanentBufferState(ShaderResource) after it); nvrhi main restores the initial state.
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        if (desc.byteSize == 0)
            desc.byteSize = sizeof(StarInstance);   // deviation: avoid creating an empty buffer when the file is missing
        device->createBuffer(desc, &m_StarBuffer);
    }

    uint32_t RealStarFieldResources::GetNumStars() const
    {
        return uint32_t(m_Stars.size());
    }

    RealStarFieldResources::~RealStarFieldResources() = default;

    void RealStarFieldResources::LoadStars(const nvrhi::AutoPtr<donut::vfs::IFileSystem>& fs, const std::filesystem::path& path)
    {
        nvrhi::AutoPtr<nvrhi::IDataBlob> blob;
        if (NVRHI_FAILED(fs->readFile(path, &blob)) || !blob)
        {
            // deviation: the 2018 code silently left the star list empty.
            donut::log::warning("Couldn't load the star catalogue '%s'", path.generic_string().c_str());
            return;
        }

        m_Stars.resize(blob->GetSize() / sizeof(StarInstance));
        memcpy(m_Stars.data(), blob->GetDataPtr(), m_Stars.size() * sizeof(StarInstance));
        m_NeedsUpload = true;
    }

    void RealStarFieldResources::UploadIfNeeded(nvrhi::ICommandList* commandList)
    {
        if (!m_NeedsUpload || m_Stars.empty())
            return;

        commandList->writeBuffer(m_StarBuffer, m_Stars.data(), m_Stars.size() * sizeof(StarInstance));
        m_NeedsUpload = false;
    }

    // ---------------------------------------------------------------------------------------------
    // RealStarFieldPass
    // ---------------------------------------------------------------------------------------------

    float3 RealStarFieldPass::ComputeCelestialParameters(const ObserverTime& time, const ObserverLocation& location)
    {
        // Julian date (Meeus), Gregorian correction from 1582-10-04.
        int year = time.year;
        int month = time.month;
        if (month < 3)
        {
            year -= 1;
            month += 12;
        }

        int b = 0;
        if (year > 1582 || (year == 1582 && (month > 10 || (month == 10 && time.day >= 4))))
        {
            const int a = year / 100;
            b = a / 4 - a + 2;
        }

        const double d = double(time.hour) / 24.0
            + double(time.day)
            + double(time.minute) / 1440.0
            + double(time.second) / 86400.0
            + double(int(double(year + 4716) * 365.25) + int(double(month + 1) * 30.6001))
            + double(b)
            - 1524.5
            - 2451545.0;     // days since J2000.0

        // Greenwich mean sidereal time, degrees.
        const double t = d / 36525.0;
        double gmst = 280.46061837 + 360.98564736629 * d + 0.000387933 * t * t - t * t * t / 38710000.0;
        if (gmst < 0.0 || gmst >= 360.0)
        {
            double turns = double(int(gmst / 360.0));
            if (gmst < 0.0)
                turns -= 1.0;
            gmst -= turns * 360.0;
        }

        const float gmstHours = float(gmst * (1.0 / 15.0));
        const float latitude = location.latitude * 0.017453292f;

        float3 result;
        result.x = gmstHours * 0.2617994f - location.longitude * 0.017453292f;  // local sidereal time, radians
        result.y = sinf(latitude);
        result.z = cosf(latitude);
        return result;
    }

    RealStarFieldPass::RealStarFieldPass(
        nvrhi::IDevice* device,
        const nvrhi::AutoPtr<ShaderFactory>& shaderFactory,
        const nvrhi::AutoPtr<CommonRenderPasses>& commonPasses,
        const nvrhi::AutoPtr<FramebufferFactory>& framebufferFactory,
        const ICompositeView& compositeView,
        const std::shared_ptr<RealStarFieldResources>& resources)
        : m_Resources(resources)
        , m_CommonPasses(commonPasses)
        , m_FramebufferFactory(framebufferFactory)
    {
        m_PixelShader = shaderFactory->CreateShader("demo/real_stars_ps.hlsl", "PS", nullptr, nvrhi::ShaderType::Pixel);
        m_VertexShader = shaderFactory->CreateShader("demo/real_stars_vs.hlsl", "VS", nullptr, nvrhi::ShaderType::Vertex);

        device->createBuffer(ConstantBufferDesc(sizeof(CelestialConstants), "CelestialConstants"), &m_CelestialConstants);

        const IView* sampleView = compositeView.GetChildView(ViewType::PLANAR, 0);

        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Vertex;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0)
        };
        device->createBindingLayout(layoutDesc, &m_BindingLayout);

        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_CelestialConstants),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_Resources->GetBuffer())   // t_StarInstances
        };
        device->createBindingSet(setDesc, m_BindingLayout, &m_BindingSet);

        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
        pipelineDesc.VS = m_VertexShader;
        pipelineDesc.PS = m_PixelShader;
        pipelineDesc.bindingLayouts = { m_BindingLayout };
        pipelineDesc.renderState.rasterState.setCullNone();
        pipelineDesc.renderState.depthStencilState
            .enableDepthTest()
            .disableDepthWrite()
            .disableStencil()
            .setDepthFunc(sampleView->IsReverseDepth()
                ? nvrhi::ComparisonFunc::GreaterOrEqual
                : nvrhi::ComparisonFunc::LessOrEqual);
        // Additive (2018 blend helper with One, One).
        pipelineDesc.renderState.blendState.targets[0] = BlendStateRT(nvrhi::BlendFactor::One, nvrhi::BlendFactor::One);

        device->createGraphicsPipeline2(pipelineDesc, m_FramebufferFactory->GetFramebuffer(*sampleView), &m_Pipeline);
    }

    void RealStarFieldPass::Render(
        nvrhi::ICommandList* commandList,
        const ICompositeView& compositeView,
        float brightness) const
    {
        demo::ProfBegin(commandList, "Environment Map");

        m_Resources->UploadIfNeeded(commandList);

        const float3 celestial = ComputeCelestialParameters(c_ObserverTime, c_ObserverLocation);

        for (uint32_t viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
        {
            const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);

            nvrhi::GraphicsState state;
            state.pipeline = m_Pipeline;
            state.framebuffer = m_FramebufferFactory->GetFramebuffer(*view);
            state.bindings = { m_BindingSet };
            state.viewport = view->GetViewportState();

            // Rotation-only view (camera at the origin) followed by the projection. The equatorial frame
            // is used directly as the world frame: ST / SinLAT / CosLAT are written but not used by the shader.
            affine3 viewMatrix = view->GetViewMatrix();
            viewMatrix.m_translation = 0.f;
            const float4x4 projection = view->GetProjectionMatrix(true);

            CelestialConstants constants{};
            constants.ViewProjectionToClip = affineToHomogeneous(viewMatrix) * projection;
            constants.ST = celestial.x;
            constants.SinLAT = celestial.y;
            constants.CosLAT = celestial.z;
            const float2 scale = demo::ProjectionScale(projection);     // an aspect ratio: magnitudes
            constants.Aspect = scale.y / scale.x;
            constants.Brightness = brightness;
            commandList->writeBuffer(m_CelestialConstants, &constants, sizeof(constants));

            commandList->setGraphicsState(state);

            nvrhi::DrawArguments args;
            args.vertexCount = 3 * REAL_STARS_PER_INSTANCE;                       // 192
            args.instanceCount = m_Resources->GetNumStars() / REAL_STARS_PER_INSTANCE;  // 258944 / 64 = 4046
            commandList->draw(args);
        }

        demo::ProfEnd(commandList);
    }
}

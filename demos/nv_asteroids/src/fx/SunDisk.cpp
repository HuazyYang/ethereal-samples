#include "fx/SunDisk.h"
#include "fx/FxCommon.h"
#include "fx/RectPass.h"
#include "scene/Lights.h"

#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/View.h>

#include <cstddef>

using namespace donut::math;
#include <space_cb.h>

using namespace donut::engine;

namespace fx
{
    SunDisk::SunDisk(
        nvrhi::IDevice* device,
        const nvrhi::AutoPtr<ShaderFactory>& shaderFactory,
        const nvrhi::AutoPtr<FramebufferFactory>& framebufferFactory,
        const ICompositeView& compositeView)
    {
        device->createBuffer(ConstantBufferDesc(sizeof(SunConstants), "SunConstants"), &m_SunConstants);

        nvrhi::BindingLayoutDesc pixelLayoutDesc;
        pixelLayoutDesc.bindings = { nvrhi::BindingLayoutItem::VolatileConstantBuffer(0) };

        // Premultiplied alpha: the shader outputs (color * I, I).
        m_RectPass = std::make_unique<RectPass>(device, shaderFactory, "demo/sun_disk_ps.hlsl", pixelLayoutDesc,
            BlendStateRT(nvrhi::BlendFactor::One, nvrhi::BlendFactor::InvSrcAlpha), framebufferFactory, compositeView);

        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, m_SunConstants) };
        device->createBindingSet(bindingSetDesc, m_RectPass->GetPixelBindingLayout(), &m_BindingSet);
    }

    SunDisk::~SunDisk() = default;

    void SunDisk::Render(
        nvrhi::ICommandList* commandList,
        const ICompositeView& compositeView,
        const SceneDirectionalLight& sun,
        float distance,
        float brightness) const
    {
        SunConstants constants{};
        constants.color = sun.color * sun.irradiance * 10.f * brightness;
        commandList->writeBuffer(m_SunConstants, &constants, sizeof(constants));

        for (uint32_t viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
        {
            const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);

            const float3 directionToSun = -normalize(sun.direction);

            // The quad covers twice the sun's angular size: the glow extends beyond the disk.
            m_RectPass->Render(commandList, *view, directionToSun, sun.angularSize * 2.f, distance, m_BindingSet);
        }
    }
}

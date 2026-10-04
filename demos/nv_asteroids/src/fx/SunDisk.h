#pragma once

#include <nvrhi/nvrhi.h>

#include <memory>

class SceneDirectionalLight;

namespace donut::engine
{
    class ShaderFactory;
    class FramebufferFactory;
    class ICompositeView;
}

namespace fx
{
    class RectPass;

    // Asteroids.exe: SunDisk (ctor 0x140006C80, Render 0x140008E80; no vtable, 24-byte object
    // {RectPass*, SunConstants buffer, binding set}, FeatureDemo+1168)
    //
    // Draws the sun as a premultiplied-alpha soft disk (demo/sun_disk_ps.hlsl) on a RectPass quad
    // twice the light's angular size, at the far plane, around the direction to the sun.
    class SunDisk
    {
    public:
        // FeatureDemo::CreateRenderPasses: (device, shaderFactory (+120), render targets' framebuffer
        // factory (targets+120), view (+608)).
        SunDisk(
            nvrhi::IDevice* device,
            const nvrhi::AutoPtr<donut::engine::ShaderFactory>& shaderFactory,
            const nvrhi::AutoPtr<donut::engine::FramebufferFactory>& framebufferFactory,
            const donut::engine::ICompositeView& compositeView);
        ~SunDisk();

        // FeatureDemo::RenderLightingAndEffects: (commandList (+696), view (+608), sun (+984),
        // g_MaxSceneDistance, 1.0f).
        void Render(
            nvrhi::ICommandList* commandList,
            const donut::engine::ICompositeView& compositeView,
            const SceneDirectionalLight& sun,
            float distance,
            float brightness) const;

    private:
        std::unique_ptr<RectPass> m_RectPass;
        nvrhi::BufferHandle m_SunConstants;
        nvrhi::BindingSetHandle m_BindingSet;
    };
}

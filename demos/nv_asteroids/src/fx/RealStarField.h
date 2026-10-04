#pragma once

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <filesystem>
#include <memory>
#include <vector>

struct StarInstance;

namespace donut::engine
{
    class ShaderFactory;
    class CommonRenderPasses;
    class FramebufferFactory;
    class ICompositeView;
}

namespace donut::vfs
{
    class IFileSystem;
}

namespace fx
{
    // Asteroids.exe: RealStarFieldResources (RTTI std::_Ref_count_obj<RealStarFieldResources>, ctor 0x140066BF0,
    // file loader 0x1400670E0). Created once in the FeatureDemo constructor (FeatureDemo +808):
    //   std::make_shared<RealStarFieldResources>(device, rootFs, mediaPath)
    //
    // Holds the star catalogue <media>/SkyAndStars/Stars.buf (258944 StarInstance records of 16 bytes:
    // Ra, Dec in degrees, visual magnitude, SAO number; see shaders/demo/space_shaders.NOTES.md) in a
    // CPU copy and in the structured buffer "Star instance buffer". The upload is deferred to the first
    // render (m_NeedsUpload).
    class RealStarFieldResources
    {
    public:
        RealStarFieldResources(
            nvrhi::IDevice* device,
            const nvrhi::AutoPtr<donut::vfs::IFileSystem>& fs,
            const std::filesystem::path& mediaPath);
        ~RealStarFieldResources();  // out of line: StarInstance is incomplete here

        // Uploads the catalogue on the first call (2018: inlined at the start of the star render).
        void UploadIfNeeded(nvrhi::ICommandList* commandList);

        [[nodiscard]] nvrhi::IBuffer* GetBuffer() const { return m_StarBuffer; }
        // Out of line: StarInstance is incomplete here.
        [[nodiscard]] uint32_t GetNumStars() const;

    private:
        void LoadStars(const nvrhi::AutoPtr<donut::vfs::IFileSystem>& fs, const std::filesystem::path& path);

        nvrhi::BufferHandle m_StarBuffer;
        std::vector<StarInstance> m_Stars;
        bool m_NeedsUpload = false;
    };

    // Asteroids.exe: real star field pass (ctor 0x140066020, Render 0x1400671F0; no vtable). FeatureDemo member +824.
    //
    // Draws every catalogue star as a camera-facing triangle (demo/real_stars_vs.hlsl VS,
    // demo/real_stars_ps.hlsl PS), additively at the far plane: 192 vertices (64 stars) per instance,
    // numStars / 64 instances. The 2018 render reuses the "Environment Map" marker.
    class RealStarFieldPass
    {
    public:
        // Observer date (2018-08-01 12:00:00, UTC-8) and location (121.93 W, 37.35 N: Santa Clara)
        // hard-coded in Asteroids.exe at 0x14025B868 / 0x14025B888.
        struct ObserverTime
        {
            int year;
            int month;
            int day;
            float hour;
            float minute;
            float second;
        };
        struct ObserverLocation
        {
            float longitude;    // degrees, positive west (subtracted from the local sidereal time)
            float latitude;     // degrees
        };

        static constexpr ObserverTime c_ObserverTime = { 2018, 8, 1, 12.f, 0.f, 0.f };
        static constexpr ObserverLocation c_ObserverLocation = { 121.93f, 37.35f };

        // 0x140066ED0: x = local sidereal time (radians), y = sin(latitude), z = cos(latitude).
        static donut::math::float3 ComputeCelestialParameters(const ObserverTime& time, const ObserverLocation& location);

        RealStarFieldPass(
            nvrhi::IDevice* device,
            const nvrhi::AutoPtr<donut::engine::ShaderFactory>& shaderFactory,
            const nvrhi::AutoPtr<donut::engine::CommonRenderPasses>& commonPasses,
            const nvrhi::AutoPtr<donut::engine::FramebufferFactory>& framebufferFactory,
            const donut::engine::ICompositeView& compositeView,
            const std::shared_ptr<RealStarFieldResources>& resources);

        // FeatureDemo::RenderLightingAndEffects, when UIData+160: Render(cmd, view, UIData+164).
        void Render(
            nvrhi::ICommandList* commandList,
            const donut::engine::ICompositeView& compositeView,
            float brightness) const;

    private:
        std::shared_ptr<RealStarFieldResources> m_Resources;
        nvrhi::ShaderHandle m_PixelShader;
        nvrhi::ShaderHandle m_VertexShader;
        nvrhi::BufferHandle m_CelestialConstants;
        nvrhi::BindingLayoutHandle m_BindingLayout;
        nvrhi::BindingSetHandle m_BindingSet;
        nvrhi::GraphicsPipelineHandle m_Pipeline;
        nvrhi::AutoPtr<donut::engine::CommonRenderPasses> m_CommonPasses;
        nvrhi::AutoPtr<donut::engine::FramebufferFactory> m_FramebufferFactory;
    };
}

// Asteroids.exe: FeatureDemo constructor (0x1400230B0, "FeatureDemo::Init") and lifetime helpers.

#include "app/FeatureDemo.h"
#include "app/AppGlobals.h"
#include "app/GpuProfiler.h"
#include "app/RenderTargets.h"
#include "app/ThirdPersonCamera.h"
#include "app/UIData.h"
#include "fs/SQLiteFileSystem.h"
#include "fx/RealStarField.h"
#include "meshlets/PipelineStatisticsQuery.h"
#include "passes/HbaoPlusPass.h"


#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/render/CascadedShadowMap.h>

#include <chrono>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

using namespace donut;
using namespace donut::math;

namespace
{
    // Password of media.db. The binary assembles it as a wide string on the stack and converts it to
    // the ANSI code page before passing it to SQLiteFileSystem.
    const wchar_t c_MediaDatabasePassword[] = L"HjLxk8CwekjjquQM";

    std::string ToAnsi(const wchar_t* text)
    {
        const int size = WideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr);
        std::string result(size_t(std::max(size, 1)), '\0');
        WideCharToMultiByte(CP_ACP, 0, text, -1, result.data(), size, nullptr, nullptr);
        result.resize(strlen(result.c_str()));
        return result;
    }

    int64_t NowNanoseconds()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Developer aid (deviation): reads from 'primary' and falls back to 'secondary'. Used by -shaderOverride to
    // run individual original shader blobs in place of the rebuilt ones.
    class OverlayFileSystem : public vfs::IFileSystem
    {
    public:
        OverlayFileSystem(std::shared_ptr<vfs::IFileSystem> primary, std::shared_ptr<vfs::IFileSystem> secondary)
            : m_Primary(std::move(primary)), m_Secondary(std::move(secondary)) {}

        bool folderExists(const std::filesystem::path& name) override
        { return m_Primary->folderExists(name) || m_Secondary->folderExists(name); }
        bool fileExists(const std::filesystem::path& name) override
        { return m_Primary->fileExists(name) || m_Secondary->fileExists(name); }
        std::shared_ptr<vfs::IBlob> readFile(const std::filesystem::path& name) override
        {
            if (auto blob = m_Primary->readFile(name))
            {
                log::info("shaderOverride: using %s", name.generic_string().c_str());
                return blob;
            }
            return m_Secondary->readFile(name);
        }
        bool writeFile(const std::filesystem::path&, const void*, size_t) override { return false; }
        int enumerateFiles(const std::filesystem::path& path, const std::vector<std::string>& extensions,
            vfs::enumerate_callback_t callback, bool allowDuplicates) override
        { return m_Secondary->enumerateFiles(path, extensions, callback, allowDuplicates); }
        int enumerateDirectories(const std::filesystem::path& path, vfs::enumerate_callback_t callback, bool allowDuplicates) override
        { return m_Secondary->enumerateDirectories(path, callback, allowDuplicates); }

    private:
        std::shared_ptr<vfs::IFileSystem> m_Primary, m_Secondary;
    };

    // Compiled shader directories. The build provides ASTEROIDS_DONUT_SHADER_DIR / ASTEROIDS_SHADER_DIR;
    // when they do not exist (e.g. a copied executable) fall back to <exe dir>/shaders/....
    std::filesystem::path FindShaderDirectory(const char* configured, const std::filesystem::path& fallback)
    {
        std::error_code ec;
        if (configured && *configured && std::filesystem::is_directory(configured, ec))
            return configured;
        return fallback;
    }
}

FeatureDemo::FeatureDemo(donut::app::DeviceManager* deviceManager, UIData* uiData, bool asyncLoad)
    : ApplicationBase(deviceManager)
    , m_DeviceManager(deviceManager)
    , m_UI(uiData)
{
    m_ConstructionTimestamp = NowNanoseconds();

    nvrhi::IDevice* device = deviceManager->GetDevice();
    const std::filesystem::path exeDirectory = donut::app::GetDirectoryWithExecutable();

    // media.db next to the executable, encrypted, opened read-only.
    const std::filesystem::path databasePath = exeDirectory / "media.db";
    auto mediaFs = std::make_shared<SQLiteFileSystem>(databasePath, true, ToAnsi(c_MediaDatabasePassword));
    if (!mediaFs->isOpen())
    {
        log::error("Couldn't open the media database file.");
        ExitProcess(1);
    }

    // deviation: the 2018 code used the SQLite file system as the root (+136) and loaded precompiled
    // shader blobs from "shaders/framework" and "shaders/demo" in it. The rebuilt shaders come from
    // ShaderMake output on disk, so a donut RootFileSystem combines:
    //   /media              -> media.db (entries are stored as "media/<name>")
    //   /shaders/donut      -> donut's compiled shaders ("donut/passes/..." resolve here)
    //   /shaders/asteroids  -> the demo's compiled shaders
    //   /shaders/demo       -> <asteroids shader dir>/demo, for CreateShader("demo/<file>.hlsl", ...)
    //   /shaders/framework  -> <asteroids shader dir>/framework (rebuilt 2018 framework passes)
#ifdef ASTEROIDS_DONUT_SHADER_DIR
    const char* configuredDonutShaders = ASTEROIDS_DONUT_SHADER_DIR;
#else
    const char* configuredDonutShaders = nullptr;
#endif
#ifdef ASTEROIDS_SHADER_DIR
    const char* configuredDemoShaders = ASTEROIDS_SHADER_DIR;
#else
    const char* configuredDemoShaders = nullptr;
#endif
    const std::filesystem::path donutShaderDir = FindShaderDirectory(configuredDonutShaders,
        exeDirectory / "shaders" / "donut" / donut::app::GetShaderTypeName(nvrhi::GraphicsAPI::D3D12));
    const std::filesystem::path demoShaderDir = FindShaderDirectory(configuredDemoShaders,
        exeDirectory / "shaders" / "asteroids" / donut::app::GetShaderTypeName(nvrhi::GraphicsAPI::D3D12));

    auto rootFs = std::make_shared<vfs::RootFileSystem>();
    rootFs->mount("/media", std::make_shared<vfs::RelativeFileSystem>(mediaFs, "media"));
    rootFs->mount("/shaders/donut", donutShaderDir);
    rootFs->mount("/shaders/asteroids", demoShaderDir);
    if (!demo::g_Options.shaderOverrideDir.empty())
    {
        const std::filesystem::path overrideDir = demo::g_Options.shaderOverrideDir;
        auto native = std::make_shared<vfs::NativeFileSystem>();
        auto overlay = [&](const char* sub)
        {
            return std::make_shared<OverlayFileSystem>(
                std::make_shared<vfs::RelativeFileSystem>(native, overrideDir / sub),
                std::make_shared<vfs::RelativeFileSystem>(native, demoShaderDir / sub));
        };
        rootFs->mount("/shaders/demo", overlay("demo"));
        rootFs->mount("/shaders/framework", overlay("framework"));
        log::info("Shader override directory: %s", overrideDir.generic_string().c_str());
    }
    else
    {
        rootFs->mount("/shaders/demo", demoShaderDir / "demo");
        rootFs->mount("/shaders/framework", demoShaderDir / "framework");
    }
    m_RootFs = rootFs;
    m_MediaPath = "/media";

    m_ShaderFactory = std::make_shared<engine::ShaderFactory>(device, m_RootFs, "/shaders");
    m_CommonPasses = std::make_shared<engine::CommonRenderPasses>(device, m_ShaderFactory);
    m_CommandList = device->createCommandList();
    if (demo::g_Options.gpuProfile)
        demo::GpuProfiler::Get().Enable(device);

    m_UI->aaMode = (demo::g_Options.antiAliasing == 1) ? AntiAliasingMode::TemporalAA : AntiAliasingMode::None;

    // deviation: donut's CascadedShadowMap takes no shader factory / common passes, and the 2018 extra
    // parameter block {4.0f, 1000, 1} has no counterpart (unresolved meaning). 2048 px, 3 cascades,
    // 1 per-object shadow, D24S8 (2018 format 47) are kept.
    m_ShadowMap = std::make_shared<render::CascadedShadowMap>(device, 2048, 3, 1, nvrhi::Format::D24S8);

    m_HbaoPlus = std::make_shared<HbaoPlusPass>(device);

    m_TextureCache = std::make_shared<engine::TextureCache>(device, m_RootFs, nullptr);
    m_TextureCache->SetGenerateMipmaps(demo::g_Options.enableMipmaps);
    m_TextureCache->SetMaxTextureSize(0);

    SetAsynchronousLoadingEnabled(asyncLoad);

    {
        nvrhi::TextureDesc desc;
        desc.width = 128;
        desc.height = 1;
        desc.depth = 1;
        desc.arraySize = 1;
        desc.mipLevels = 1;
        desc.format = nvrhi::Format::R32_FLOAT;         // 2018 format 33
        desc.dimension = nvrhi::TextureDimension::Texture1D;
        desc.debugName = "ViewDistanceMap";
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        m_ViewDistanceMap = device->createTexture(desc);
    }

    m_PipelineStatsQuery = std::make_shared<PipelineStatisticsQuery>(m_DeviceManager->GetDevice(), 3);

    if (demo::g_Options.replayIndex >= 0)
        LoadReplay(demo::g_Options.replayIndex);
    LoadAnimations();

    // unresolved: the "-scene" option is parsed by WinMain but the constructor always loads this name.
    BeginLoadingScene(m_RootFs, m_MediaPath / "asteroids.fscene");

    LoadCameraPresets(c_NumCameraPresets);

    m_RealStarFieldResources = std::make_shared<fx::RealStarFieldResources>(device, m_RootFs, m_MediaPath);

    CameraPreset preset;
    if (demo::g_Options.viewIndex >= 0 && size_t(demo::g_Options.viewIndex) < m_CameraPresets.size())
    {
        preset = m_CameraPresets[demo::g_Options.viewIndex];
        m_UI->cameraMode = 0;
        m_UI->showGui = false;
    }

    m_FirstPersonCamera.LookAt(preset.position, preset.lookAt, preset.up);
    m_ActiveCamera = &m_FirstPersonCamera;
    m_FirstPersonCamera.SetMoveSpeed(100.f);
}

FeatureDemo::~FeatureDemo()
{
    // deviation: donut's ApplicationBase does not join a still-running loading thread; the binary
    // avoided the situation by calling ExitProcess when the window closed during loading.
    if (m_SceneLoadingThread && m_SceneLoadingThread->joinable())
        m_SceneLoadingThread->join();
}

void FeatureDemo::PlayOpeningSequence()
{
    m_ActiveSequence = &m_OpeningSequence;
    m_OpeningSequence.Restart();
}

void FeatureDemo::ResetOpeningSequence()
{
    m_ActiveSequence = nullptr;
    ApplyAnimation(m_OpeningSequence, m_OpeningSequence.GetDuration(), true);
}

// Asteroids.exe: FeatureDemo data files.
//   replay.%d.json 0x1400310B0, animations.json 0x140030580, animation application 0x14002AE20,
//   Camera%d.json load 0x140030760 / save 0x140035810, light probes load 0x140030D40 / save 0x140035D40,
//   texture saving 0x1400360F0.

#include "app/FeatureDemo.h"
#include "app/AppGlobals.h"
#include "app/JsonFile.h"
#include "app/ThirdPersonCamera.h"
#include "app/UIData.h"
#include "scene/PlayerShip.h"
#include "scene/SpaceScene.h"

#include <donut/core/log.h>
#include <donut/core/json.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/DDSFile.h>
#include <donut/engine/SceneTypes.h>
#include <donut/engine/TextureCache.h>

#include <json/json.h>

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace donut;
using namespace donut::math;

void FeatureDemo::LoadReplay(int index)
{
    char fileName[256];
    snprintf(fileName, sizeof(fileName), "replay.%d.json", index);

    Json::Value root;
    if (demo::LoadJsonFile(*m_RootFs, m_MediaPath / fileName, root, true))
        m_Replay.Load(root);
}

void FeatureDemo::LoadAnimations()
{
    Json::Value root;
    if (demo::LoadJsonFile(*m_RootFs, m_MediaPath / "animations.json", root, true))
    {
        m_OpeningSequence.Load(root["start"]);
        m_OpeningSequenceFast.Load(root["start_fast"]);
    }
    m_ActiveSequence = nullptr;
}

void FeatureDemo::ApplyAnimation(const Animation::Sequence& sequence, std::optional<float> time, bool holdLastValue)
{
    if (auto sunRotation = sequence.Evaluate<float>("SunRotation", time, holdLastValue))
        m_UI->sunRotation = *sunRotation;

    const auto shipPosition = sequence.Evaluate<float3>("PlayerShipPosition", time, holdLastValue);
    const auto shipRotation = sequence.Evaluate<float2>("PlayerShipRotation", time, holdLastValue);
    const auto shipSpeed = sequence.Evaluate<float>("PlayerShipSpeed", time, holdLastValue);

    PlayerShip* ship = m_Scene ? m_Scene->GetPlayerShip() : nullptr;

    // Rotations are authored in degrees.
    if (ship && shipPosition && shipRotation)
        ship->SetPose(*shipPosition, radians(shipRotation->x), radians(shipRotation->y), 0.f);

    if (ship && shipSpeed)
        ship->SetSpeed(*shipSpeed);

    const auto cameraDistance = sequence.Evaluate<float>("ThirdPersonCameraDistance", time, holdLastValue);
    const auto cameraRotation = sequence.Evaluate<float2>("ThirdPersonCameraRotation", time, holdLastValue);

    if ((cameraDistance || cameraRotation) && m_ShipCamera)
    {
        if (cameraDistance)
            m_ShipCamera->SetDistance(*cameraDistance);
        if (cameraRotation)
            m_ShipCamera->SetRotation(radians(cameraRotation->x), radians(cameraRotation->y));
        m_ShipCamera->ClearHistory();
    }
}

void FeatureDemo::LoadCameraPresets(int count)
{
    m_CameraPresets.reserve(size_t(count));

    for (int index = 0; index < count; index++)
    {
        char fileName[256];
        snprintf(fileName, sizeof(fileName), "Camera%d.json", index);

        // Missing presets are skipped silently, so the vector only holds the files that exist.
        Json::Value root;
        if (!demo::LoadJsonFile(*m_RootFs, m_MediaPath / fileName, root, false))
            continue;

        CameraPreset preset;
        preset.position = json::Read<float3>(root["pos"], float3(0.f));
        preset.lookAt = json::Read<float3>(root["look_at"], float3(0.f, 0.f, -1.f));
        preset.up = json::Read<float3>(root["up"], float3(0.f, 1.f, 0.f));
        preset.valid = true;
        m_CameraPresets.push_back(preset);
    }
}

void FeatureDemo::SaveCameraPreset(int index)
{
    // deviation: the binary writes m_CameraPresets[index] without a bounds check.
    if (size_t(index) >= m_CameraPresets.size())
        m_CameraPresets.resize(size_t(index) + 1);

    CameraPreset& preset = m_CameraPresets[size_t(index)];
    preset.position = m_FirstPersonCamera.GetPosition();
    preset.lookAt = m_FirstPersonCamera.GetPosition() + m_FirstPersonCamera.GetDir();
    preset.up = m_FirstPersonCamera.GetUp();
    preset.valid = true;

    // Written with std::ofstream to the current directory (not into media.db).
    std::ostringstream fileName;
    fileName << "Camera" << index << ".json";

    std::ofstream file(fileName.str());
    if (!file.good())
        return;

    file << "{\n";
    file << "  \"pos\": [ " << preset.position.x << ", " << preset.position.y << ", " << preset.position.z << " ],\n";
    file << "  \"look_at\": [ " << preset.lookAt.x << ", " << preset.lookAt.y << ", " << preset.lookAt.z << " ],\n";
    file << "  \"up\": [ " << preset.up.x << ", " << preset.up.y << ", " << preset.up.z << " ]\n";
    file << "}\n";
}

void FeatureDemo::LoadLightProbes(engine::ThreadPool& threadPool)
{
    const engine::TextureLoadOptions linear{ engine::SRGBModeFromBool(false) };

    m_LightProbeDiffuse = m_TextureCache->LoadTextureFromFileAsync(m_MediaPath / "LightProbeDiffuse.dds", linear, threadPool);
    m_LightProbeSpecular = m_TextureCache->LoadTextureFromFileAsync(m_MediaPath / "LightProbeSpecular.dds", linear, threadPool);
    m_EnvironmentBrdf = m_TextureCache->LoadTextureFromFileAsync(m_MediaPath / "EnvironmentBrdf.dds", linear, threadPool);
}

void FeatureDemo::SaveLightProbes()
{
    // Called after rendering the probes with -renderLightProbes. The files go through the root file
    // system, i.e. into media.db.
    // unresolved: SQLiteFileSystem::writeFile is not implemented (the 2018 database was opened read-only
    // as well), so the save reports "Failed to write texture to ..." unless /media is remounted writable.
    const std::pair<const std::shared_ptr<engine::LoadedTexture>*, const char*> outputs[] = {
        { &m_LightProbeDiffuse, "LightProbeDiffuse.dds" },
        { &m_LightProbeSpecular, "LightProbeSpecular.dds" },
        { &m_EnvironmentBrdf, "EnvironmentBrdf.dds" },
    };

    // Developer option (deviation): "-probeOutput DIR" also writes the files to a native folder.
    std::shared_ptr<vfs::IFileSystem> nativeFs;
    if (!demo::g_Options.probeOutputDir.empty())
    {
        std::filesystem::create_directories(demo::g_Options.probeOutputDir);
        nativeFs = std::make_shared<vfs::NativeFileSystem>();
    }

    for (const auto& [texture, fileName] : outputs)
    {
        if (!*texture || !(*texture)->texture)
            continue;
        SaveTextureToFile((*texture)->texture, m_RootFs, m_MediaPath / fileName);
        if (nativeFs)
            SaveTextureToFile((*texture)->texture, nativeFs, std::filesystem::path(demo::g_Options.probeOutputDir) / fileName);
    }
}

void FeatureDemo::SaveTextureToFile(nvrhi::ITexture* texture, const std::shared_ptr<vfs::IFileSystem>& fs,
    const std::filesystem::path& path)
{
    nvrhi::IDevice* device = GetDevice();
    const nvrhi::TextureDesc& desc = texture->getDesc();

    nvrhi::StagingTextureHandle staging = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);

    m_CommandList->open();
    for (uint32_t arraySlice = 0; arraySlice < desc.arraySize; arraySlice++)
    {
        for (uint32_t mipLevel = 0; mipLevel < desc.mipLevels; mipLevel++)
        {
            const nvrhi::TextureSlice slice = nvrhi::TextureSlice().setArraySlice(arraySlice).setMipLevel(mipLevel);
            m_CommandList->copyTexture(staging, slice, texture, slice);
        }
    }
    m_CommandList->close();
    device->executeCommandList(m_CommandList);

    std::shared_ptr<vfs::IBlob> blob = engine::SaveStagingTextureAsDDS(device, staging);
    if (!blob)
    {
        log::error("Failed to serialize a texture for saving");
        return;
    }

    if (!fs->writeFile(path, blob->data(), blob->size()))
        log::error("Failed to write texture to %s", path.generic_string().c_str());
}

#pragma once

// The demo scene: asteroids.fscene loader (lights, models, player ship, asteroid field, controls,
// sounds), the sector grid with asteroid placements, PhysX-based ship collision and the scene-wide
// material / mesh lists.
//
// Asteroids.exe: SpaceScene (vtable 0x14025B040, 0x1C8 bytes; ctor 0x14005A010, dtor 0x14005A700,
// Load 0x14005C890, LoadScene 0x14005D9D0, ResolveCollision 0x14005B6D0).

#include "scene/AsteroidLibrary.h"
#include "scene/SceneGraph.h"
#include "scene/SceneMaterial.h"
#include "scene/SpaceSector.h"

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Json { class Value; }
namespace donut::vfs { class IFileSystem; }
namespace donut::engine { class CommonRenderPasses; class TextureCache; class ThreadPool; }
namespace audio { class Sound; }
namespace physx { class PxFoundation; class PxPhysics; class PxCooking; }

class CargoShip;
class PlayerShip;
class SceneLight;
class SpaceObject;

class SpaceScene
{
public:
    explicit SpaceScene(std::shared_ptr<donut::vfs::IFileSystem> fs);
    ~SpaceScene();
    SpaceScene(const SpaceScene&) = delete;
    SpaceScene& operator=(const SpaceScene&) = delete;

    // 0x14005C890: creates the scene and loads 'sceneFile'; returns null on failure.
    // Textures are only queued: the caller must let the TextureCache finish
    // (ProcessRenderingThreadCommands + LoadingFinished) before CreateRenderingResources.
    static std::shared_ptr<SpaceScene> Load(std::shared_ptr<donut::vfs::IFileSystem> fs,
        const std::filesystem::path& sceneFile, donut::engine::TextureCache* textureCache,
        donut::engine::ThreadPool* threadPool = nullptr);

    bool LoadScene(const std::filesystem::path& sceneFile, donut::engine::TextureCache* textureCache,
        donut::engine::ThreadPool* threadPool = nullptr);

    // vfunc00..02: scene-wide lists gathered after loading (objects first, then asteroid-type materials).
    const std::vector<SceneMesh*>& GetMeshes() const { return m_Meshes; }
    const std::vector<SceneMeshInstance*>& GetMeshInstances() const { return m_MeshInstances; }
    // Every material has materialID = index + 1 (used by material-id picking).
    const std::vector<SceneMaterial*>& GetMaterials() const { return m_Materials; }

    // vfunc03: GPU resources of every object (meshlet or vertex/index path) and every asteroid type.
    void CreateRenderingResources(nvrhi::IDevice* device, std::shared_ptr<donut::engine::CommonRenderPasses> commonPasses,
        bool useMeshlets);
    // 0x14005B3D0: per-sector "Instances" buffers (FeatureDemo calls it whenever
    // AreRenderingResourcesCreated() is false, e.g. after LoadPlacement()).
    void CreateResources(nvrhi::IDevice* device);
    bool AreRenderingResourcesCreated() const { return m_RenderingResourcesCreated; }

    // 0x14005FDD0: upload pending sector instance data.
    void Update(nvrhi::ICommandList* commandList);
    // 0x14005FE50: per-object material constant updates (ship glows).
    void Animate(nvrhi::ICommandList* commandList);
    // 0x14005FF00: union of the objects' bounds.
    void UpdateBounds();
    // Re-reads the placement file and rebuilds the sectors (0x14005D830).
    bool LoadPlacement();

    // 0x14005B6D0: sweeps a capsule of 'radius' from 'from' towards 'to' against the asteroids of the
    // 3x3 sectors around 'to' and returns the reachable position.
    donut::math::float3 ResolveCollision(const donut::math::float3& from, const donut::math::float3& to, float radius);

    // Objects (models and the player ship's SpaceObject, which is the last one)
    const std::vector<std::shared_ptr<SpaceObject>>& GetObjects() const { return m_Objects; }
    std::shared_ptr<SpaceObject> GetSpaceObject(size_t index) const;   // 0x14005C7B0 (null when out of range)
    const donut::math::box3& GetBounds() const { return m_Bounds; }

    PlayerShip* GetPlayerShip() const { return m_PlayerShip.get(); }
    CargoShip* GetCargoShip() const { return m_CargoShip.get(); }

    // Lights: fscene "lights" plus the ship's glow and nav lights. FeatureDemo adds a fallback sun.
    std::vector<std::shared_ptr<SceneLight>>& GetLights() { return m_Lights; }
    const std::vector<std::shared_ptr<SceneLight>>& GetLights() const { return m_Lights; }

    const donut::math::float3& GetSunColour() const { return m_SunColour; }
    const donut::math::float3& GetAmbientColour() const { return m_AmbientColour; }
    // fscene "color_lut", as written in the file (FeatureDemo joins it with the media directory).
    const std::filesystem::path& GetColorLut() const { return m_ColorLut; }
    // Looping ambience music; created stopped (FeatureDemo starts it unless muted). May be null.
    audio::Sound* GetAmbienceMusic() const { return m_AmbienceMusic.get(); }

    // Asteroid field
    AsteroidLibrary& GetAsteroidLibrary() { return *m_AsteroidLibrary; }
    const AsteroidLibrary& GetAsteroidLibrary() const { return *m_AsteroidLibrary; }
    const std::vector<std::shared_ptr<SpaceSector>>& GetSectors() const { return m_Sectors; }
    const donut::math::int2& GetNumSectors() const { return m_NumSectors; }
    const donut::math::float3& GetSectorSize() const { return m_SectorSize; }
    const donut::math::float3& GetWorldMin() const { return m_WorldMin; }
    const donut::math::float3& GetWorldMax() const { return m_WorldMax; }
    float GetWorldDiagonal() const;                                         // 0x14005C740
    donut::math::int2 GetSectorIndex(const donut::math::float3& position) const;   // 0x14005FD60 (unwrapped)
    std::shared_ptr<SpaceSector> GetSector(const donut::math::int2& index) const;  // 0x140060060 (wrapped)
    donut::math::float3 GetSectorOrigin(const donut::math::int2& index) const;     // 0x14005C830 (unwrapped)

    nvrhi::IBindingLayout* GetMaterialBindingLayout() const { return m_MaterialBindingLayout; }

    // 0x14005C720: progress of the current load (read by the loading screen from another thread).
    static SceneLoadProgress& GetLoadProgress();
    // 0x14005FC80
    static void SetSoundMuted(bool mute);

private:
    bool LoadLights(const Json::Value& lights);
    bool LoadModels(const Json::Value& models, const std::filesystem::path& directory,
        donut::engine::TextureCache* textureCache, donut::engine::ThreadPool* threadPool);
    void LoadInstances(const Json::Value& instances, SpaceObject& object);
    bool LoadAsteroids(const Json::Value& asteroids, const std::filesystem::path& directory,
        donut::engine::TextureCache* textureCache, donut::engine::ThreadPool* threadPool);
    bool ParsePlacement(const Json::Value& placement);
    void GatherSceneLists();
    int WrapSectorX(int x) const;
    int WrapSectorZ(int z) const;

    std::shared_ptr<donut::vfs::IFileSystem> m_FS;
    std::vector<std::shared_ptr<SpaceObject>> m_Objects;
    donut::math::box3 m_Bounds = donut::math::box3::empty();
    donut::math::float3 m_SunColour = 1.f;
    donut::math::float3 m_AmbientColour = 1.f;
    std::vector<std::shared_ptr<SpaceSector>> m_Sectors;            // index = x + numSectors.x * z
    std::unique_ptr<AsteroidLibrary> m_AsteroidLibrary;
    donut::math::float3 m_WorldMin = -1000.f;
    donut::math::float3 m_WorldMax = 1000.f;
    donut::math::float3 m_SectorSize = donut::math::float3(200.f, 50.f, 200.f);
    donut::math::int2 m_NumSectors = donut::math::int2(3, 3);
    std::vector<SceneMesh*> m_Meshes;
    std::vector<SceneMeshInstance*> m_MeshInstances;
    std::vector<SceneMaterial*> m_Materials;
    std::unique_ptr<PlayerShip> m_PlayerShip;
    std::unique_ptr<CargoShip> m_CargoShip;
    std::filesystem::path m_PlacementFile;
    std::filesystem::path m_ColorLut;
    bool m_RenderingResourcesCreated = true;
    nvrhi::BindingLayoutHandle m_MaterialBindingLayout;

    physx::PxFoundation* m_Foundation = nullptr;
    physx::PxPhysics* m_Physics = nullptr;
    physx::PxCooking* m_Cooking = nullptr;

    std::unique_ptr<audio::Sound> m_AmbienceMusic;
    std::vector<std::shared_ptr<SceneLight>> m_Lights;
};

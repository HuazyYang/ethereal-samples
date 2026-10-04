#include "scene/SpaceScene.h"
#include "scene/CargoShip.h"
#include "scene/Lights.h"
#include "scene/PlayerShip.h"
#include "scene/SpaceObject.h"

#include "audio/SoundEngine.h"

#include <donut/core/json.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/ThreadPool.h>

#include <json/json.h>

#include <PxPhysicsAPI.h>
#include <characterkinematic/PxCapsuleController.h>
#include <characterkinematic/PxControllerManager.h>

#include <atomic>
#include <cmath>
#include <malloc.h>

using namespace donut;
using namespace donut::math;

// ------------------------------------------------------------------------------------------------
// PhysX glue
//
// deviation: the 2018 executable statically linked PhysX3Extensions (PxDefaultAllocator,
// PxDefaultErrorCallback, PxDefaultCpuDispatcherCreate, PxDefaultSimulationFilterShader,
// PxInitExtensions). Only the PhysX core / cooking / character-kinematic DLLs ship with the demo,
// so minimal equivalents are provided here. The collision query never calls simulate(), so the
// dispatcher runs tasks inline.

namespace
{
    class PhysXAllocator : public physx::PxAllocatorCallback
    {
    public:
        void* allocate(size_t size, const char*, const char*, int) override { return _aligned_malloc(size, 16); }
        void deallocate(void* ptr) override { _aligned_free(ptr); }
    };

    class PhysXErrorCallback : public physx::PxErrorCallback
    {
    public:
        void reportError(physx::PxErrorCode::Enum code, const char* message, const char* file, int line) override
        {
            if (code == physx::PxErrorCode::eDEBUG_INFO)
                log::info("PhysX: %s (%s:%d)", message, file, line);
            else if (code == physx::PxErrorCode::eDEBUG_WARNING || code == physx::PxErrorCode::ePERF_WARNING)
                log::warning("PhysX: %s (%s:%d)", message, file, line);
            else
                log::error("PhysX: %s (%s:%d)", message, file, line);
        }
    };

    class InlineCpuDispatcher : public physx::PxCpuDispatcher
    {
    public:
        void submitTask(physx::PxBaseTask& task) override
        {
            task.run();
            task.release();
        }
        uint32_t getWorkerCount() const override { return 0; }
    };

    physx::PxFilterFlags SimulationFilterShader(physx::PxFilterObjectAttributes attributes0, physx::PxFilterData,
        physx::PxFilterObjectAttributes attributes1, physx::PxFilterData, physx::PxPairFlags& pairFlags,
        const void*, physx::PxU32)
    {
        // Same behaviour as PxDefaultSimulationFilterShader without group masks.
        if (physx::PxFilterObjectIsTrigger(attributes0) || physx::PxFilterObjectIsTrigger(attributes1))
        {
            pairFlags = physx::PxPairFlag::eTRIGGER_DEFAULT;
            return physx::PxFilterFlags();
        }
        pairFlags = physx::PxPairFlag::eCONTACT_DEFAULT;
        return physx::PxFilterFlags();
    }

    PhysXAllocator g_PhysXAllocator;
    PhysXErrorCallback g_PhysXErrorCallback;
    InlineCpuDispatcher g_CpuDispatcher;

    SceneLoadProgress g_LoadProgress;   // qword_1402E02C0

    physx::PxVec3 ToPx(const float3& v) { return physx::PxVec3(v.x, v.y, v.z); }

    physx::PxQuat RotationToPx(const affine3& rotation)
    {
        // donut's row-vector rows are PhysX's (column-vector) columns.
        const physx::PxMat33 m(ToPx(rotation.m_linear.row0), ToPx(rotation.m_linear.row1), ToPx(rotation.m_linear.row2));
        return physx::PxQuat(m);
    }
}

// ------------------------------------------------------------------------------------------------
// Construction

SpaceScene::SpaceScene(std::shared_ptr<vfs::IFileSystem> fs)
    : m_FS(std::move(fs))
{
    // Asteroids.exe: 0x14005A010
    m_CargoShip = std::make_unique<CargoShip>();
    m_PlayerShip = std::make_unique<PlayerShip>(m_CargoShip.get());
    m_AsteroidLibrary = std::make_unique<AsteroidLibrary>();

    m_Foundation = PxCreateFoundation(PX_FOUNDATION_VERSION, g_PhysXAllocator, g_PhysXErrorCallback);
    if (!m_Foundation)
    {
        log::error("PxCreateFoundation failed");
        return;
    }

    const physx::PxTolerancesScale scale;
    m_Physics = PxCreatePhysics(PX_PHYSICS_VERSION, *m_Foundation, scale, false, nullptr);
    if (!m_Physics)
    {
        log::error("PxCreatePhysics failed");
        return;
    }

    m_Cooking = PxCreateCooking(PX_PHYSICS_VERSION, *m_Foundation, physx::PxCookingParams(scale));
    if (!m_Cooking)
        log::error("PxCreateCooking failed");
}

SpaceScene::~SpaceScene()
{
    // Asteroids.exe: 0x14005A700. Objects referencing PhysX meshes go first.
    m_PlayerShip.reset();
    m_CargoShip.reset();
    m_AsteroidLibrary.reset();
    m_Objects.clear();

    if (m_Cooking)
        m_Cooking->release();
    if (m_Physics)
        m_Physics->release();
    if (m_Foundation)
        m_Foundation->release();
}

SceneLoadProgress& SpaceScene::GetLoadProgress()
{
    return g_LoadProgress;
}

void SpaceScene::SetSoundMuted(bool mute)
{
    // Asteroids.exe: 0x14005FC80
    if (auto engine = audio::Engine::Get())
        engine->SetMute(mute);
}

std::shared_ptr<SpaceScene> SpaceScene::Load(std::shared_ptr<vfs::IFileSystem> fs, const std::filesystem::path& sceneFile,
    engine::TextureCache* textureCache, engine::ThreadPool* threadPool)
{
    // Asteroids.exe: 0x14005C890
    auto scene = std::make_shared<SpaceScene>(std::move(fs));
    if (scene->LoadScene(sceneFile, textureCache, threadPool))
        return scene;
    return nullptr;
}

// ------------------------------------------------------------------------------------------------
// Loading

bool SpaceScene::LoadScene(const std::filesystem::path& sceneFile, engine::TextureCache* textureCache,
    engine::ThreadPool* threadPool)
{
    // Asteroids.exe: 0x14005D9D0
    g_LoadProgress.total = 0;
    g_LoadProgress.completed = 0;

    const std::filesystem::path directory = sceneFile.parent_path();

    Json::Value root;
    if (!json::LoadFromFile(*m_FS, sceneFile, root))
        return false;

    bool ok = true;
    if (root.isArray())
    {
        // Legacy format: the file is just a models array.
        ok = LoadModels(root, directory, textureCache, threadPool);
    }
    else if (root.isObject())
    {
        if (root["ambienceMusic"].isString())
        {
            ++g_LoadProgress.total;
            m_AmbienceMusic = audio::Sound::Create(m_FS, directory / std::filesystem::path(root["ambienceMusic"].asString()), true);
            if (m_AmbienceMusic)
                m_AmbienceMusic->SetVolume(json::Read<float>(root["ambienceMusicVolume"], 1.f));
            else if (auto engine = audio::Engine::Get())
            {
                std::string message = "Sound engine errors :\n";
                for (const std::string& error : audio::Engine::GetErrors())
                    message += error + "\n";
                log::warning("%s", message.c_str());
            }
            else
                log::warning("Sound engine initialization failed");
            ++g_LoadProgress.completed;
        }

        // Failures do not return early: controls, sounds and colours are still read (2018 behaviour).
        if (!LoadLights(root["lights"]) || !LoadModels(root["models"], directory, textureCache, threadPool))
            ok = false;
        else
        {
            const Json::Value& playerShip = root["player_ship"];
            ++g_LoadProgress.total;
            const float3 position = json::Read<float3>(playerShip[0]["position"], float3(0.f));
            const float yaw = playerShip[0]["yaw"].asFloat();       // used as radians (no conversion in 2018)
            const float pitch = playerShip[0]["pitch"].asFloat();

            bool shipOk = LoadModels(playerShip, directory, textureCache, threadPool) && !m_Objects.empty();
            if (shipOk)
            {
                shipOk &= m_CargoShip->Init(playerShip, m_Lights, m_Objects.back());
                if (m_CargoShip->GetSpaceObject())
                    m_CargoShip->GetSpaceObject()->isPlayerShip = true;
                m_PlayerShip->SetPose(position, yaw, pitch, 0.f);
                ++g_LoadProgress.completed;
                ok = shipOk && LoadAsteroids(root["asteroids"], directory, textureCache, threadPool);
            }
            else
                ok = false;
        }

        m_PlayerShip->LoadControls(root["controls"]);
        m_PlayerShip->LoadSounds(root["player_ship"], m_FS, directory);
        m_SunColour = json::Read<float3>(root["sun_colour"], float3(1.3f));
        m_AmbientColour = json::Read<float3>(root["ambient_colour"], float3(0.1f));
        if (root["color_lut"].isString())
            m_ColorLut = std::filesystem::path(root["color_lut"].asString());
        // "version" and "camera_speed" are not read by the 2018 loader.
    }
    else
    {
        log::error("Unrecognized structure of the scene description file");
        ok = false;
    }

    GatherSceneLists();
    return ok;
}

void SpaceScene::GatherSceneLists()
{
    m_Bounds = box3::empty();
    for (const auto& object : m_Objects)
        m_Bounds |= object->GetBounds();

    m_Materials.clear();
    m_Meshes.clear();
    m_MeshInstances.clear();
    for (const auto& object : m_Objects)
    {
        m_Materials.insert(m_Materials.end(), object->GetMaterials().begin(), object->GetMaterials().end());
        m_Meshes.insert(m_Meshes.end(), object->GetMeshes().begin(), object->GetMeshes().end());
        m_MeshInstances.insert(m_MeshInstances.end(), object->GetMeshInstances().begin(), object->GetMeshInstances().end());
    }
    for (const auto& type : m_AsteroidLibrary->GetTypes())
        m_Materials.insert(m_Materials.end(), type->GetMaterials().begin(), type->GetMaterials().end());

    // deviation: null entries (unresolved material names) are dropped; the 2018 code dereferenced them.
    m_Materials.erase(std::remove(m_Materials.begin(), m_Materials.end(), nullptr), m_Materials.end());

    int materialID = 1;
    for (SceneMaterial* material : m_Materials)
        material->materialID = materialID++;
}

bool SpaceScene::LoadLights(const Json::Value& lights)
{
    // Asteroids.exe: 0x14005E970. Every default is the light's current (constructor) value.
    for (const Json::Value& node : lights)
    {
        if (!node.isObject() || !node["type"].isString())
            continue;

        const std::string type = node["type"].asString();
        if (type == "dir_light" || type == "directional")
        {
            auto light = std::make_shared<SceneDirectionalLight>();
            light->name = json::Read<std::string>(node["name"], light->name);
            light->direction = normalize(json::Read<float3>(node["direction"], light->direction));
            light->angularSize = json::Read<float>(node["angularSize"], 1.f);
            if (node["intensity"].isArray())
            {
                light->irradiance = 1.f;
                light->color = json::Read<float3>(node["intensity"], light->color);
            }
            else
            {
                light->irradiance = json::Read<float>(node["irradiance"], 1.f);
                light->color = json::Read<float3>(node["color"], light->color);
            }
            m_Lights.push_back(light);
        }
        else if (type == "point_light" || type == "point" || type == "omni")
        {
            auto light = std::make_shared<ScenePointLight>();
            light->name = json::Read<std::string>(node["name"], light->name);
            light->position = json::Read<float3>(node["position"], json::Read<float3>(node["pos"], light->position));
            light->radius = json::Read<float>(node["radius"], 0.2f);
            if (node["intensity"].isArray())
            {
                light->flux = 1.f;
                light->color = json::Read<float3>(node["intensity"], light->color);
            }
            else
            {
                light->flux = json::Read<float>(node["flux"], 1.f);
                light->color = json::Read<float3>(node["color"], light->color);
            }
            m_Lights.push_back(light);
        }
        else if (type == "spot_light" || type == "spot")
        {
            auto light = std::make_shared<SceneSpotLight>();
            light->name = json::Read<std::string>(node["name"], light->name);
            light->position = json::Read<float3>(node["position"], json::Read<float3>(node["pos"], light->position));
            light->direction = normalize(json::Read<float3>(node["direction"], light->direction));
            light->flux = json::Read<float>(node["flux"], 1.f);
            light->color = json::Read<float3>(node["color"], light->color);
            light->innerAngle = json::Read<float>(node["innerAngle"], 60.f);
            light->outerAngle = json::Read<float>(node["outerAngle"], 90.f);
            m_Lights.push_back(light);
        }
    }
    return true;
}

bool SpaceScene::LoadModels(const Json::Value& models, const std::filesystem::path& directory,
    engine::TextureCache* textureCache, engine::ThreadPool* threadPool)
{
    // Asteroids.exe: 0x14005F710 (task body 0x14005AC70)
    if (!models.isArray())
        return false;

    m_RenderingResourcesCreated = false;
    auto success = std::make_shared<std::atomic<bool>>(true);

    SpaceObjectLoadContext context;
    context.textureCache = textureCache;
    context.threadPool = threadPool;
    context.progress = &g_LoadProgress;

    for (const Json::Value& model : models)
    {
        ++g_LoadProgress.total;
        auto object = std::make_shared<SpaceObject>(m_FS);
        m_Objects.push_back(object);

        if (model.isMember("instances"))
            LoadInstances(model["instances"], *object);

        const std::filesystem::path file = directory / std::filesystem::path(model["file"].asString());
        std::filesystem::path materials = std::filesystem::path(model["materials"].asString());
        if (!materials.empty())
            materials = directory / materials;

        auto task = [object, file, materials, context, success]()
        {
            if (object->Load(file, materials, context, false, LoadFlag_Default))
                ++g_LoadProgress.completed;
            else
            {
                log::error("Error loading %s", file.generic_string().c_str());
                *success = false;
            }
        };

        // deviation: concurrency::task_group -> donut ThreadPool (or serial loading without one).
        if (threadPool)
            threadPool->AddTask(task);
        else
            task();
    }

    if (threadPool)
        threadPool->WaitForTasks();

    return *success;
}

void SpaceScene::LoadInstances(const Json::Value& instances, SpaceObject& object)
{
    // Asteroids.exe: 0x14005D420
    for (const Json::Value& node : instances)
    {
        if (node.isArray() && node.size() == 16)
        {
            float m[16];
            for (int i = 0; i < 16; ++i)
                m[i] = node[i].asFloat();
            object.AddInstanceTransform(affine3(m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14]));
        }
        else if (node.isObject())
        {
            const float3 t = json::Read<float3>(node["translation"], float3(0.f));
            const float3 s = json::Read<float3>(node["scaling"], float3(1.f));
            float3 r = json::Read<float3>(node["rotation"], float3(0.f));

            affine3 transform = affine3::identity();
            if (r.x != 0.f || r.y != 0.f || r.z != 0.f)
            {
                r *= 0.017453292f;  // fscene rotations are in degrees
                transform *= yawPitchRoll(r.x, r.y, r.z);
            }
            transform *= scaling(s);
            transform *= translation(t);
            object.AddInstanceTransform(transform);
        }
    }
}

bool SpaceScene::LoadAsteroids(const Json::Value& asteroids, const std::filesystem::path& directory,
    engine::TextureCache* textureCache, engine::ThreadPool* threadPool)
{
    // Asteroids.exe: 0x14005C9D0
    g_LoadProgress.total += 3;

    if (!asteroids.isObject() || !asteroids.isMember("library") || !asteroids.isMember("world_min")
        || !asteroids.isMember("number_of_sectors"))
    {
        log::error("Load asteroids : invalid root node");
        return false;
    }

    const std::filesystem::path libraryPath = directory / std::filesystem::path(asteroids["library"].asString());
    Json::Value library;
    if (!json::LoadFromFile(*m_FS, libraryPath, library))
        return false;

    AsteroidLoadContext context;
    context.fs = m_FS;
    context.textureCache = textureCache;
    context.threadPool = threadPool;
    context.progress = &g_LoadProgress;
    context.cooking = m_Cooking;
    context.physics = m_Physics;
    m_AsteroidLibrary->Load(library, directory, context);

    m_WorldMin = json::Read<float3>(asteroids["world_min"], float3(-1000.f));
    m_WorldMax = json::Read<float3>(asteroids["world_max"], float3(1000.f));
    m_NumSectors.x = asteroids["number_of_sectors"][0].asInt();
    m_NumSectors.y = asteroids["number_of_sectors"][1].asInt();
    // deviation: guard against zero sector counts (division by zero / modulo by zero in 2018).
    m_NumSectors = max(m_NumSectors, int2(1));
    // The sector height is the whole world height (only x and z are subdivided).
    m_SectorSize = float3(
        (m_WorldMax.x - m_WorldMin.x) / float(m_NumSectors.x),
        (m_WorldMax.y - m_WorldMin.y),
        (m_WorldMax.z - m_WorldMin.z) / float(m_NumSectors.y));

    const Json::Value& placementFile = asteroids["placement_file"];
    if (!placementFile.isString())
    {
        log::error("Load asteroids : placement_file not specified");
        return false;
    }
    m_PlacementFile = directory / std::filesystem::path(placementFile.asString());

    m_Sectors.clear();
    for (int z = 0; z < m_NumSectors.y; ++z)
        for (int x = 0; x < m_NumSectors.x; ++x)
            m_Sectors.push_back(std::make_shared<SpaceSector>(m_AsteroidLibrary->GetNumTypes()));

    LoadPlacement();    // result ignored (2018)
    ++g_LoadProgress.completed;
    return true;
}

bool SpaceScene::LoadPlacement()
{
    // Asteroids.exe: 0x14005D830
    m_RenderingResourcesCreated = false;

    Json::Value root;
    if (!json::LoadFromFile(*m_FS, m_PlacementFile, root))
        return false;

    for (auto& sector : m_Sectors)
        sector->Clear();

    ParsePlacement(root);   // result ignored (2018)

    const AsteroidLibrary& library = *m_AsteroidLibrary;
    for (auto& sector : m_Sectors)
        sector->UpdateTransformsAndBounds([&library](uint32_t typeId) { return library.GetBounds(typeId); });

    return true;
}

int SpaceScene::WrapSectorX(int x) const
{
    return (m_NumSectors.x + x % m_NumSectors.x) % m_NumSectors.x;
}

int SpaceScene::WrapSectorZ(int z) const
{
    return (m_NumSectors.y + z % m_NumSectors.y) % m_NumSectors.y;
}

bool SpaceScene::ParsePlacement(const Json::Value& placement)
{
    // Asteroids.exe: 0x14005CFB0 — asteroid-placement.json: [{ "name", "translation", "scaling", "rotation" (radians) }]
    if (!placement.isArray())
        return false;

    for (const Json::Value& node : placement)
    {
        if (!node.isObject())
            continue;

        const std::string name = node["name"].asString();
        if (name.empty())
            return false;   // aborts the whole parse (2018)

        const float3 t = json::Read<float3>(node["translation"], float3(0.f));
        const float3 s = json::Read<float3>(node["scaling"], float3(1.f));
        const float3 r = json::Read<float3>(node["rotation"], float3(0.f));

        const int2 cell = GetSectorIndex(t);
        const std::shared_ptr<SpaceSector> sector = GetSector(cell);
        const float3 origin = GetSectorOrigin(cell);

        const uint32_t typeId = m_AsteroidLibrary->GetAsteroidTypeId(name);
        if (typeId == c_InvalidAsteroidTypeId)
            return false;

        sector->AddInstance(typeId, t - origin, s, r);
    }
    return true;
}

// ------------------------------------------------------------------------------------------------
// Sector helpers

int2 SpaceScene::GetSectorIndex(const float3& position) const
{
    return int2(
        int(floorf((position.x - m_WorldMin.x) / m_SectorSize.x)),
        int(floorf((position.z - m_WorldMin.z) / m_SectorSize.z)));
}

std::shared_ptr<SpaceSector> SpaceScene::GetSector(const int2& index) const
{
    if (m_Sectors.empty())
        return nullptr;
    return m_Sectors[size_t(WrapSectorX(index.x) + m_NumSectors.x * WrapSectorZ(index.y))];
}

float3 SpaceScene::GetSectorOrigin(const int2& index) const
{
    return float3(
        m_WorldMin.x + float(index.x) * m_SectorSize.x,
        m_WorldMin.y + 0.f,
        m_WorldMin.z + float(index.y) * m_SectorSize.z);
}

float SpaceScene::GetWorldDiagonal() const
{
    return length(m_WorldMax - m_WorldMin);
}

std::shared_ptr<SpaceObject> SpaceScene::GetSpaceObject(size_t index) const
{
    return index < m_Objects.size() ? m_Objects[index] : nullptr;
}

// ------------------------------------------------------------------------------------------------
// GPU resources and per-frame updates

void SpaceScene::CreateRenderingResources(nvrhi::IDevice* device, std::shared_ptr<engine::CommonRenderPasses> commonPasses,
    bool useMeshlets)
{
    // Asteroids.exe: vfunc03 0x14005B440
    if (!m_MaterialBindingLayout)
        m_MaterialBindingLayout = CreateMaterialBindingLayout(device);

    nvrhi::CommandListHandle commandList = device->createCommandList();
    commandList->open();
    for (const auto& object : m_Objects)
        object->CreateRenderResources(device, commandList, *commonPasses, m_MaterialBindingLayout, useMeshlets, false);
    commandList->close();
    device->executeCommandList(commandList);

    m_AsteroidLibrary->CreateRenderResources(device, *commonPasses, m_MaterialBindingLayout);
}

void SpaceScene::CreateResources(nvrhi::IDevice* device)
{
    // Asteroids.exe: 0x14005B3D0
    for (auto& sector : m_Sectors)
        sector->CreateInstanceBuffers(device);
    m_RenderingResourcesCreated = true;
}

void SpaceScene::Update(nvrhi::ICommandList* commandList)
{
    // Asteroids.exe: 0x14005FDD0
    for (auto& sector : m_Sectors)
        sector->Update(commandList);
}

void SpaceScene::Animate(nvrhi::ICommandList* commandList)
{
    // Asteroids.exe: 0x14005FE50
    for (const auto& object : m_Objects)
        object->UpdateMaterials(commandList);
}

void SpaceScene::UpdateBounds()
{
    // Asteroids.exe: 0x14005FF00
    m_Bounds = box3::empty();
    for (const auto& object : m_Objects)
        m_Bounds |= object->GetBounds();
}

// ------------------------------------------------------------------------------------------------
// Collision

float3 SpaceScene::ResolveCollision(const float3& from, const float3& to, float radius)
{
    // Asteroids.exe: 0x14005B6D0 — a temporary PxScene with the nearby asteroids as static triangle
    // meshes, and a capsule controller moved from 'from' to 'to'.
    if (!m_Physics || m_Sectors.empty())
        return to;

    physx::PxPhysics& physics = *m_Physics;
    physx::PxSceneDesc sceneDesc{ physx::PxTolerancesScale() };
    sceneDesc.filterShader = SimulationFilterShader;
    sceneDesc.cpuDispatcher = &g_CpuDispatcher;
    physx::PxScene* scene = physics.createScene(sceneDesc);
    if (!scene)
        return to;

    physx::PxMaterial* material = physics.createMaterial(0.5f, 0.5f, 0.1f);

    const int2 cell = GetSectorIndex(to);
    std::vector<physx::PxRigidStatic*> actors;
    const auto& types = m_AsteroidLibrary->GetTypes();

    for (int z = cell.y - 1; z <= cell.y + 1; ++z)
    {
        for (int x = cell.x - 1; x <= cell.x + 1; ++x)
        {
            const float3 origin = GetSectorOrigin(int2(x, z));
            const std::shared_ptr<SpaceSector> sector = GetSector(int2(x, z));
            const float3 localPos = to - origin;
            const box3& bounds = sector->GetBounds();
            if (!(all(localPos >= bounds.m_mins - radius) && all(localPos <= bounds.m_maxs + radius)))
                continue;

            for (uint32_t t = 0; t < uint32_t(types.size()) && t < sector->GetNumTypes(); ++t)
            {
                physx::PxTriangleMesh* mesh = types[t]->GetCollisionMesh();
                const uint32_t count = sector->GetInstanceCount(t);
                for (uint32_t i = 0; i < count; ++i)
                {
                    const AsteroidPlacement& p = sector->GetInstance(t, i);
                    const float3 d = p.sphereCenter - localPos;
                    // Literal 2018 test (looks like a typo for (r + radius)^2; kept for fidelity).
                    if (dot(d, d) >= p.sphereRadius * p.sphereRadius + 2.f * radius)
                        continue;
                    if (!mesh)
                        continue;

                    const physx::PxTransform pose(ToPx(origin + p.position),
                        RotationToPx(yawPitchRoll(p.rotation.x, p.rotation.y, p.rotation.z)));
                    physx::PxRigidStatic* actor = physics.createRigidStatic(pose);
                    // PhysX scales before rotating; the render transform rotates first. They agree for the
                    // uniform scales used by the data.
                    const physx::PxTriangleMeshGeometry geometry(mesh,
                        physx::PxMeshScale(ToPx(p.scaling), physx::PxQuat(physx::PxIdentity)));
                    physx::PxShape* shape = physics.createShape(geometry, *material, true);
                    if (shape)
                    {
                        actor->attachShape(*shape);
                        shape->release();
                    }
                    scene->addActor(*actor);
                    actors.push_back(actor);
                }
            }
        }
    }

    float3 result = to;
    physx::PxControllerManager* manager = PxCreateControllerManager(*scene, false);
    if (manager)
    {
        physx::PxCapsuleControllerDesc desc;
        desc.position = physx::PxExtendedVec3(from.x, from.y, from.z);
        desc.upDirection = physx::PxVec3(0.f, 1.f, 0.f);
        desc.slopeLimit = 0.f;
        desc.invisibleWallHeight = 0.f;
        desc.maxJumpHeight = 0.f;
        desc.contactOffset = 0.1f;
        desc.stepOffset = 0.1f;
        desc.density = 10.f;
        desc.scaleCoeff = 0.8f;
        desc.volumeGrowth = 1.5f;
        desc.reportCallback = nullptr;
        desc.behaviorCallback = nullptr;
        desc.nonWalkableMode = physx::PxControllerNonWalkableMode::ePREVENT_CLIMBING;
        desc.material = material;
        desc.registerDeletionListener = true;
        desc.userData = nullptr;
        desc.radius = radius;
        desc.height = 1e-6f;
        desc.climbingMode = physx::PxCapsuleClimbingMode::eEASY;

        physx::PxController* controller = manager->createController(desc);
        if (controller)
        {
            controller->move(ToPx(to - from), 0.f, 1.f / 60.f, physx::PxControllerFilters(), nullptr);
            const physx::PxExtendedVec3 position = controller->getPosition();
            result = float3(float(position.x), float(position.y), float(position.z));
            controller->release();
        }
        manager->release();
    }

    material->release();
    scene->release();
    for (physx::PxRigidStatic* actor : actors)
        actor->release();

    return result;
}

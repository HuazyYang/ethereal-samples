#include "scene/CargoShip.h"
#include "scene/Lights.h"
#include "scene/SceneGraph.h"
#include "scene/SceneMaterial.h"
#include "scene/SpaceObject.h"

#include <donut/core/json.h>

#include <json/json.h>

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <sstream>

using namespace donut;
using namespace donut::math;

namespace
{
    constexpr size_t c_GlowNoiseSize = 1000;
    float g_GlowNoise[c_GlowNoiseSize];     // flt_1402DF2B0
    bool g_GlowNoiseInitialized = false;    // byte_1402E0250 (plain bool, not a thread-safe static)

    void InitGlowNoise()
    {
        // A sequence of 20 sine segments with random periods (~50 samples) and amplitudes;
        // deterministic (default-seeded mt19937_64).
        std::mt19937_64 rng;
        std::uniform_real_distribution<float> periodDist(0.8f, 1.2f);
        std::uniform_real_distribution<float> amplitudeDist(0.f, 1.f);

        size_t period = size_t(50.f / periodDist(rng));
        float amplitude = amplitudeDist(rng);
        size_t index = 0;
        for (unsigned j = 0; j != 20; ++j)
        {
            // deviation: bounds guard (the 2018 loop trusts the random periods to stay below 1000 in total).
            for (size_t k = 0; k != period && index < c_GlowNoiseSize; ++k)
                g_GlowNoise[index++] = sinf(float(int(k)) * 6.2831855f / float(period)) * amplitude;

            amplitude = amplitudeDist(rng);
            if (j >= 18)
                period = c_GlowNoiseSize - index;
            else
                period = size_t(50.f / periodDist(rng));
        }
    }
}

ShipGlow::ShipGlow()
{
    // Asteroids.exe: 0x140047910
    static std::mt19937_64 rng;
    static std::uniform_int_distribution<size_t> dist(0, 1000);
    noisePhase = dist(rng);
}

bool ShipRotatingPart::Init(const Json::Value& node, const SpaceObject& object)
{
    // Asteroids.exe: 0x140049B50
    if (!node.isMember("name"))
        return false;

    name = node["name"].asString();
    axis = normalize(json::Read<float3>(node["axis"], float3(1.f, 0.f, 0.f)));
    this->node = object.GetRootNode()->Find(name, "");
    if (!this->node)
        return false;

    std::stringstream ss;
    ss << "Rotating part " << this->node;
    OutputDebugStringA(ss.str().c_str());
    return true;
}

bool CargoShip::Init(const Json::Value& playerShipArray, std::vector<std::shared_ptr<SceneLight>>& sceneLights,
    std::shared_ptr<SpaceObject> object)
{
    // Asteroids.exe: 0x140049580
    m_SpaceObject = std::move(object);
    m_RootNode = m_SpaceObject->GetRootNode();

    if (playerShipArray.size() != 1)
        return false;

    for (const Json::Value& partNode : playerShipArray[0]["rotating_parts"])
    {
        ShipRotatingPart part;
        part.Init(partNode, *m_SpaceObject);    // pushed even when the node is not found (2018)
        m_RotatingParts.push_back(std::move(part));
    }

    m_ShipNode = m_RootNode->Find("CargoShip", "");
    if (m_ShipNode)
        m_ShipNode->SetTransform(translation(float3(0.f, -15.f, 0.f)));

    InitGlows(sceneLights, *m_SpaceObject);
    InitNavLights(sceneLights);
    return true;
}

bool CargoShip::InitGlows(std::vector<std::shared_ptr<SceneLight>>& sceneLights, const SpaceObject& object)
{
    // Asteroids.exe: 0x140048380
    const float3 rearOffset(0.f, 0.f, -1.5f);
    InitGlow(m_Glows[4], sceneLights, object, "Glow_RR_T", rearOffset);
    InitGlow(m_Glows[5], sceneLights, object, "Glow_RR_B", rearOffset);
    InitGlow(m_Glows[6], sceneLights, object, "Glow_RL_T", rearOffset);
    InitGlow(m_Glows[7], sceneLights, object, "Glow_RL_B", rearOffset);
    InitGlow(m_Glows[0], sceneLights, object, "Glow_FR_T", float3(0.f));
    InitGlow(m_Glows[1], sceneLights, object, "Glow_FR_B", float3(0.f));
    InitGlow(m_Glows[2], sceneLights, object, "Glow_FL_T", float3(0.f));
    InitGlow(m_Glows[3], sceneLights, object, "Glow_FL_B", float3(0.f));

    if (!g_GlowNoiseInitialized)
    {
        g_GlowNoiseInitialized = true;
        InitGlowNoise();
    }
    return true;
}

void CargoShip::InitGlow(ShipGlow& glow, std::vector<std::shared_ptr<SceneLight>>& sceneLights, const SpaceObject& object,
    const std::string& name, const float3& offset)
{
    // Asteroids.exe: 0x140049190
    glow.lightOffset = offset;
    glow.node = object.GetRootNode()->Find(name, "");

    for (SceneMeshInstance* instance : object.GetMeshInstances())
    {
        if (instance->name == name)
        {
            glow.instance = instance;
            break;
        }
    }

    if (glow.node)
    {
        glow.light = std::make_shared<ScenePointLight>();
        glow.light->name = name;
        glow.light->flux = 0.f;
        glow.light->color = float3(0.f, 1.2f, 2.f);
        glow.light->radius = 1.f;
        sceneLights.push_back(glow.light);
    }
}

bool CargoShip::InitNavLights(std::vector<std::shared_ptr<SceneLight>>& sceneLights)
{
    // Asteroids.exe: 0x140048D20
    auto top = std::make_shared<ScenePointLight>();
    top->name = "TopNavLight";
    top->flux = 0.f;
    top->color = float3(1.f, 1.f, 1.f);
    top->radius = 1.f;
    top->range = 4.f;
    m_NavLights.push_back(top);
    sceneLights.push_back(top);

    auto stern = std::make_shared<ScenePointLight>();
    stern->name = "SternNavLight";
    stern->flux = 0.f;
    stern->color = float3(1.f, 0.f, 0.f);
    stern->radius = 1.f;
    stern->range = 3.5f;
    m_NavLights.push_back(stern);
    sceneLights.push_back(stern);
    return true;
}

void CargoShip::SetNavLightState(const float3& state)
{
    // Asteroids.exe: 0x14004A020
    if (m_SpaceObject)
    {
        m_SpaceObject->shipGlowParams.y = state.x;
        m_SpaceObject->shipGlowParams.z = state.y;
        m_SpaceObject->shipGlowParams.w = state.z;
    }
}

void CargoShip::SetTransform(const affine3& transform)
{
    // Asteroids.exe: 0x14004A040
    if (m_RootNode)
        m_RootNode->SetTransform(transform);
}

const affine3& CargoShip::GetTransform() const
{
    // Asteroids.exe: 0x1400490B0
    static const affine3 s_Identity = affine3::identity();
    return m_RootNode ? m_RootNode->GetTransform() : s_Identity;
}

void CargoShip::Update(float time, float /*dt*/)
{
    // Asteroids.exe: 0x140047DB0 — the noise table is sampled at 250 samples per second.
    const size_t t = size_t(time / 0.2f * (float(c_GlowNoiseSize) / 20.f));

    for (ShipGlow& glow : m_Glows)
    {
        glow.noise = g_GlowNoise[(t + glow.noisePhase) % c_GlowNoiseSize];

        if (glow.node)
        {
            const float s = std::min(glow.intensity + 0.7f, 1.f);
            const float sz = (glow.noise * 0.03f + 1.f) * glow.intensity;
            glow.node->SetTransform(scaling(float3(s, s, sz)));
        }

        if (glow.instance && glow.instance->mesh && glow.instance->mesh->material)
        {
            SceneMaterial* material = glow.instance->mesh->material;
            const float opacity = glow.intensity * 0.12f;
            if (material->opacity != opacity)
            {
                material->opacity = opacity;
                material->dirty = true;
            }
        }
    }

    if (m_RootNode)
        m_RootNode->UpdateTransforms(affine3::identity());

    for (ShipGlow& glow : m_Glows)
    {
        if (!glow.light)
            continue;
        glow.light->flux = (glow.noise * 0.3f + 1.f) * glow.intensity;
        if (glow.node)
            glow.light->position = glow.node->GetWorldTransform().transformPoint(glow.lightOffset);
    }

    // deviation: the 2018 code dereferences the "CargoShip" node without a null check.
    if (m_ShipNode && m_SpaceObject)
    {
        const affine3& shipWorld = m_ShipNode->GetWorldTransform();
        if (m_NavLights.size() > 0)
        {
            m_NavLights[0]->position = shipWorld.transformPoint(float3(0.f, 17.896f, 5.684f));
            m_NavLights[0]->flux = m_SpaceObject->shipGlowParams.z * 4.f;
        }
        if (m_NavLights.size() > 1)
        {
            m_NavLights[1]->position = shipWorld.transformPoint(float3(0.f, 11.44f, -19.22f));
            m_NavLights[1]->flux = m_SpaceObject->shipGlowParams.y * 6.f;
        }
    }

    if (m_SpaceObject)
    {
        const float rear = 0.f + m_Glows[4].intensity * 0.2f + m_Glows[5].intensity * 0.2f
            + m_Glows[6].intensity * 0.2f + m_Glows[7].intensity * 0.2f;
        m_SpaceObject->shipGlowParams.x = (rear * (m_Glows[4].noise * 0.4f + 1.f)) * 0.25f;
    }
}

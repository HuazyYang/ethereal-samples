#pragma once

// Visual side of the player's cargo ship: scene-graph nodes of the ship model, 8 engine glows (each
// with a point light and noise-driven flicker), 2 blinking nav lights and the rotating-part list.
//
// Asteroids.exe: CargoShip (0x2A0 bytes, constructed inline in the SpaceScene ctor 0x14005A010, owned at
// SpaceScene+264; Init 0x140049580, glows 0x140048380 / 0x140049190, nav lights 0x140048D20,
// Update 0x140047DB0, rotating part 0x140049B50). No RTTI exists; the name is from the "CargoShip" node.

#include <donut/core/math/math.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace Json { class Value; }

class SceneLight;
class ScenePointLight;
class SceneNode;
class SpaceObject;
struct SceneMeshInstance;

// One engine glow (72 bytes; ctor 0x140047910). Index order in CargoShip:
// 0 Glow_FR_T, 1 Glow_FR_B, 2 Glow_FL_T, 3 Glow_FL_B, 4 Glow_RR_T, 5 Glow_RR_B, 6 Glow_RL_T, 7 Glow_RL_B.
struct ShipGlow
{
    ShipGlow();

    donut::math::float3 lightOffset = 0.f;      // light position in glow-node space
    float intensity = 0.f;
    float noise = 0.f;                          // current flicker sample
    size_t noisePhase = 0;                      // random offset into the noise table
    std::shared_ptr<ScenePointLight> light;
    std::shared_ptr<SceneNode> node;
    SceneMeshInstance* instance = nullptr;      // glow geometry (its material opacity follows the intensity)
};

// fscene "rotating_parts" entry (64 bytes). Loaded but never animated by the 2018 demo
// (the fscene notes "Local xforms are broken in the latest").
struct ShipRotatingPart
{
    donut::math::float3 axis = donut::math::float3(1.f, 0.f, 0.f);
    std::string name;
    std::shared_ptr<SceneNode> node;

    bool Init(const Json::Value& node, const SpaceObject& object);
};

class CargoShip
{
public:
    static constexpr int c_NumGlows = 8;

    // Initializes the ship from the fscene "player_ship" array (exactly one entry) and the ship's
    // SpaceObject. The glow and nav lights are appended to 'sceneLights'.
    bool Init(const Json::Value& playerShipArray, std::vector<std::shared_ptr<SceneLight>>& sceneLights,
        std::shared_ptr<SpaceObject> object);

    // Glow flicker/scale, glow material opacity, scene-graph transforms, light positions and fluxes.
    void Update(float time, float dt);

    void SetGlowIntensity(int index, float intensity) { m_Glows[index].intensity = intensity; }
    // Writes the nav-light blink states into SpaceObject::shipGlowParams.yzw.
    void SetNavLightState(const donut::math::float3& state);
    // Sets the root node's transform (model offset * orientation * position).
    void SetTransform(const donut::math::affine3& transform);
    const donut::math::affine3& GetTransform() const;

    SpaceObject* GetSpaceObject() const { return m_SpaceObject.get(); }
    const std::shared_ptr<SpaceObject>& GetSpaceObjectPtr() const { return m_SpaceObject; }
    const std::array<ShipGlow, c_NumGlows>& GetGlows() const { return m_Glows; }
    const std::vector<std::shared_ptr<ScenePointLight>>& GetNavLights() const { return m_NavLights; }
    const std::vector<ShipRotatingPart>& GetRotatingParts() const { return m_RotatingParts; }

private:
    bool InitGlows(std::vector<std::shared_ptr<SceneLight>>& sceneLights, const SpaceObject& object);
    void InitGlow(ShipGlow& glow, std::vector<std::shared_ptr<SceneLight>>& sceneLights, const SpaceObject& object,
        const std::string& name, const donut::math::float3& offset);
    bool InitNavLights(std::vector<std::shared_ptr<SceneLight>>& sceneLights);

    std::array<ShipGlow, c_NumGlows> m_Glows;
    std::shared_ptr<SceneNode> m_RootNode;
    std::shared_ptr<SceneNode> m_ShipNode;          // "CargoShip"
    std::shared_ptr<SpaceObject> m_SpaceObject;
    std::vector<ShipRotatingPart> m_RotatingParts;
    std::vector<std::shared_ptr<ScenePointLight>> m_NavLights;  // [0] TopNavLight, [1] SternNavLight
};

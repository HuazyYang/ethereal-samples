#pragma once

// 2018 free-standing light objects (not scene-graph leaves).
//
// deviation: the 2018 framework lights (Light / DirectionalLight / SpotLight / PointLight) store
// their own position and direction, use a "flux" intensity convention, and are owned directly by
// SpaceScene::m_Lights and by the cargo ship (engine glows, nav lights). donut main's
// donut::engine::Light is a SceneGraphLeaf whose position comes from its node and whose intensity
// is photometric, so mapping them would need a SceneGraph and different constants. They are kept
// as small demo-side classes that fill the 2018 LightConstants (light2018::, 96 bytes, see
// shaders/demo/include/light_cb.h) with the 2018 formulas.

#include <donut/core/math/math.h>

#include <memory>
#include <string>

namespace light2018 { struct LightConstants; } // 2018 layout (shaders/demo/include/light_cb.h), not donut's

namespace donut::engine
{
    class IShadowMap;
}

// Asteroids.exe: Light (vtable 0x140257DA0; vfunc0 GetLightType, vfunc1 FillLightConstants)
class SceneLight
{
public:
    std::string name;
    std::shared_ptr<donut::engine::IShadowMap> shadowMap;
    donut::math::float3 color = 1.f;

    virtual ~SceneLight() = default;
    virtual int GetLightType() const = 0;
    virtual void FillLightConstants(light2018::LightConstants& lightConstants) const;
};

// Asteroids.exe: DirectionalLight (vtable 0x140257DB8)
class SceneDirectionalLight : public SceneLight
{
public:
    donut::math::float3 direction = donut::math::float3(0.f, -1.f, 0.f);
    float irradiance = 1.f;
    float angularSize = 1.f;    // degrees

    int GetLightType() const override;
    void FillLightConstants(light2018::LightConstants& lightConstants) const override;
};

// Asteroids.exe: SpotLight (vtable 0x140257DD0)
class SceneSpotLight : public SceneLight
{
public:
    donut::math::float3 position = 0.f;
    donut::math::float3 direction = donut::math::float3(0.f, -1.f, 0.f);
    float flux = 1.f;
    float radius = 0.01f;
    float range = 10.f;
    float innerAngle = 60.f;    // degrees
    float outerAngle = 90.f;    // degrees

    int GetLightType() const override;
    void FillLightConstants(light2018::LightConstants& lightConstants) const override;
};

// Asteroids.exe: PointLight (vtable 0x140257DE8, make_shared 0x140047540: flux 1, radius 0.2, range 10)
class ScenePointLight : public SceneLight
{
public:
    donut::math::float3 position = 0.f;
    float flux = 1.f;
    float radius = 0.2f;
    float range = 10.f;

    int GetLightType() const override;
    void FillLightConstants(light2018::LightConstants& lightConstants) const override;
};

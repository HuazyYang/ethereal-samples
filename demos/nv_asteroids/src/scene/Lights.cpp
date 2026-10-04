#include "scene/Lights.h"

#include <donut/engine/ShadowMap.h>

#include <algorithm>

using namespace donut::math;
#include <light_cb.h> // 2018 layout
#include <donut/shaders/light_types.h>

void SceneLight::FillLightConstants(light2018::LightConstants& lightConstants) const
{
    lightConstants.color = color;
    lightConstants.shadowCascades = int4(-1);
    lightConstants.perObjectShadows = int4(-1);
    lightConstants.outOfBoundsShadow = shadowMap ? (shadowMap->IsLitOutOfBounds() ? 1.f : 0.f) : 1.f;
}

int SceneDirectionalLight::GetLightType() const
{
    return LightType_Directional;
}

void SceneDirectionalLight::FillLightConstants(light2018::LightConstants& lightConstants) const
{
    SceneLight::FillLightConstants(lightConstants);

    lightConstants.lightType = LightType_Directional;
    lightConstants.direction = normalize(direction);
    const float angle = radians(std::clamp(angularSize, 0.1f, 90.f));
    lightConstants.angularSizeOrInvRange = angle;
    lightConstants.radiance = irradiance / (1.f - cosf(angle * 0.5f));
}

int SceneSpotLight::GetLightType() const
{
    return LightType_Spot;
}

void SceneSpotLight::FillLightConstants(light2018::LightConstants& lightConstants) const
{
    SceneLight::FillLightConstants(lightConstants);

    lightConstants.lightType = LightType_Spot;
    lightConstants.direction = normalize(direction);
    lightConstants.position = position;
    lightConstants.radius = radius;
    lightConstants.angularSizeOrInvRange = range > 0.f ? 1.f / range : 0.f;
    const float r = radius * PI_f;
    lightConstants.radiance = flux / (8.f * r * r);
    lightConstants.innerAngle = radians(innerAngle);
    lightConstants.outerAngle = radians(outerAngle);
}

int ScenePointLight::GetLightType() const
{
    return LightType_Point;
}

void ScenePointLight::FillLightConstants(light2018::LightConstants& lightConstants) const
{
    SceneLight::FillLightConstants(lightConstants);

    lightConstants.lightType = LightType_Point;
    lightConstants.position = position;
    lightConstants.radius = radius;
    lightConstants.angularSizeOrInvRange = range > 0.f ? 1.f / range : 0.f;
    const float r = radius * PI_f;
    lightConstants.radiance = flux * 100.f / (8.f * r * r);
}

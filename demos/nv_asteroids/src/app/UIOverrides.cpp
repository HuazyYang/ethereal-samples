#include "app/UIOverrides.h"
#include "app/UIData.h"

#include <donut/core/log.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace
{
    enum class Kind { Bool, Int, Float };

    struct Field
    {
        const char* name;
        Kind kind;
        size_t offset;
    };

#define UI_FIELD(kind, name) { #name, Kind::kind, offsetof(UIData, name) }

    const Field c_Fields[] = {
        UI_FIELD(Bool, enableHbao), UI_FIELD(Bool, taaEnableHistoryClamping),
        UI_FIELD(Float, taaNewFrameWeight), UI_FIELD(Float, taaClampingFactor),
        UI_FIELD(Int, alphaPreset), UI_FIELD(Float, sharpness), UI_FIELD(Bool, enableVsync),
        UI_FIELD(Bool, enableBloom), UI_FIELD(Float, bloomSigma),
        UI_FIELD(Bool, enableShadows), UI_FIELD(Bool, wireframe), UI_FIELD(Bool, dynamicLod),
        UI_FIELD(Bool, asteroidsCulling), UI_FIELD(Bool, hiZCulling), UI_FIELD(Bool, distanceLod),
        UI_FIELD(Int, zCullSectorThreshold), UI_FIELD(Float, cameraLodBias), UI_FIELD(Float, shadowLodBias),
        UI_FIELD(Float, lodScale), UI_FIELD(Bool, enableToneMapping), UI_FIELD(Float, nebulaBrightness),
        UI_FIELD(Bool, enableRealStars), UI_FIELD(Float, realStarBrightness), UI_FIELD(Bool, useLightProbeAmbient),
        UI_FIELD(Bool, useProbeHeightBands), UI_FIELD(Float, sunDiffuseScale), UI_FIELD(Float, sunSpecularScale),
        UI_FIELD(Int, staticLodIndex), UI_FIELD(Float, lodTransitionRange), UI_FIELD(Bool, visualizeLods),
        UI_FIELD(Float, shadowDistance), UI_FIELD(Float, shadowDepthRange), UI_FIELD(Float, cascadeExponent),
        UI_FIELD(Bool, shadowFitToView), UI_FIELD(Float, verticalFov), UI_FIELD(Float, minAsteroidScreenSize),
        UI_FIELD(Int, cameraMode), UI_FIELD(Bool, enableFog), UI_FIELD(Bool, enableParticles),
        UI_FIELD(Bool, enableLensFlare), UI_FIELD(Float, planetLightScale), UI_FIELD(Bool, drawAsteroids),
        UI_FIELD(Bool, drawSpaceObjects), UI_FIELD(Bool, enableShield),
    };

#undef UI_FIELD
}

bool ApplyUIOverride(UIData& ui, const std::string& assignment)
{
    const size_t equals = assignment.find('=');
    if (equals == std::string::npos)
    {
        donut::log::warning("-set: expected field=value, got '%s'", assignment.c_str());
        return false;
    }

    const std::string name = assignment.substr(0, equals);
    const std::string text = assignment.substr(equals + 1);

    for (const Field& field : c_Fields)
    {
        if (name != field.name)
            continue;

        char* base = reinterpret_cast<char*>(&ui) + field.offset;
        char* end = nullptr;
        switch (field.kind)
        {
        case Kind::Bool:
        {
            const bool value = !(text == "0" || text == "false" || text == "off");
            *reinterpret_cast<bool*>(base) = value;
            donut::log::info("-set %s = %s", field.name, value ? "true" : "false");
            return true;
        }
        case Kind::Int:
        {
            const long value = strtol(text.c_str(), &end, 10);
            if (end == text.c_str())
                break;
            *reinterpret_cast<int32_t*>(base) = int32_t(value);
            donut::log::info("-set %s = %ld", field.name, value);
            return true;
        }
        case Kind::Float:
        {
            const float value = strtof(text.c_str(), &end);
            if (end == text.c_str())
                break;
            *reinterpret_cast<float*>(base) = value;
            donut::log::info("-set %s = %g", field.name, double(value));
            return true;
        }
        }
        donut::log::warning("-set: cannot parse '%s' for %s", text.c_str(), field.name);
        return false;
    }

    donut::log::warning("-set: unknown setting '%s'", name.c_str());
    return false;
}

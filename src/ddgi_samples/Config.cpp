#include "Config.h"
#include <donut/core/vfs/VFS.h>
#include <donut/core/json.h>

static bool ParseConfigs(const Json::Value &docRoot, const std::filesystem::path &relPath, Config &config) {
    config.configFileDir = relPath.string();

    // scene
    auto &scene = docRoot["scene"];
    {
        std::string filePath;
        scene["filePath"] >> filePath;
        config.scene.filePath = (relPath / filePath).string();
        scene["skyRadiance"] >> config.scene.skyRadiance;
    }

    // Interaction
    auto &interaction = docRoot["interaction"];
    {
        interaction["mouse.movementSpeed"] >> config.interaction.mouse.movementSpeed;
        interaction["mouse.rotationSpeed"] >> config.interaction.mouse.rotationSpeed;
        interaction["mouse.invertPan"] >> config.interaction.mouse.invertPan;
    }

    auto &renderers = docRoot["renderers"];
    {
        auto renderMode = renderers["renderMode"].as<std::string>();
        {
            if (renderMode == "DDGI")
                config.renderers.renderMode = Config::Renderers::RenderMode::DDGI;
            else
                config.renderers.renderMode = Config::Renderers::RenderMode::PathTracing;
        }

        auto &pathTracing = renderers["pathTracing"];
        {
            pathTracing["rayNormalBias"] >> config.renderers.pt.rayNormalBias;
            pathTracing["rayViewBias"] >> config.renderers.pt.rayViewBias;
            pathTracing["numBounces"] >> config.renderers.pt.numBounces;
            pathTracing["samplesPerPixel"] >> config.renderers.pt.samplersPerPixel;
            pathTracing["antiailiasing"] >> config.renderers.pt.antialiasing;
        }

        auto &rtao = renderers["rtao"];
        {
            rtao["enabled"] >> config.renderers.rtao.enabled;
            rtao["rayLength"] >> config.renderers.rtao.rayLength;
            rtao["rayNormalBias"] >> config.renderers.rtao.rayNormalBias;
            rtao["rayViewBias"] >> config.renderers.rtao.rayViewBias;
            rtao["powerLog"] >> config.renderers.rtao.powerLog;
            rtao["filterDepthSigma"] >> config.renderers.rtao.filterDepthSigma;
            rtao["filterDistanceSigma"] >> config.renderers.rtao.filterDistanceSigma;
        }

        auto &postProcessing = renderers["postProcessing"];
        {
            postProcessing["exposure"]["enabled"] >> config.renderers.pp.exposure.enabled;
            postProcessing["exposure"]["fstops"] >> config.renderers.pp.exposure.fstops;
            postProcessing["tonemapping.enabled"] >>
                config.renderers.pp.tonemapping.enabled;
            postProcessing["dithering.enabled"] >> config.renderers.pp.dithering.enabled;
            postProcessing["gammaCorrection.enabled"] >>
                config.renderers.pp.gammaCorrection.enabled;
        }

        auto &ddgi = renderers["DDGI"];
        {
            config.renderers.ddgi.enabled = true;

            auto &children = ddgi["children"];

            for(auto &ddgiVolume : children) {
                Config::Renderers::DDGI::DDGIVolume vol;
                ddgiVolume["name"] >> vol.name;
                ddgiVolume["probeRelocation.enabled"] >> vol.probeRelocationEnabled;
                ddgiVolume["probeRelocation.minFrontfaceDistance"] >>
                    vol.probeRelocationMinFrontfaceDistance;
                ddgiVolume["probeClassification.enabled"] >> vol.probeClassificationEnabled;
                ddgiVolume["probeVariability.enabled"] >> vol.probeVariabilityEnabled;
                ddgiVolume["probeVariability.threshold"] >> vol.probeVariabilityThreshold;
                ddgiVolume["infiniteScrolling.enabled"] >> vol.infiniteScrollingEnabled;
                ddgiVolume["textures.rayData.format"] >> vol.texturesRayDataFormat;
                ddgiVolume["textures.irradiance.format"] >> vol.texturesIrradianceFormat;
                ddgiVolume["textures.distance.format"] >> vol.texturesDistanceFormat;
                ddgiVolume["textures.data.format"] >> vol.texturesDataFormat;
                ddgiVolume["textures.variability.format"] >> vol.texturesVariabilityFormat;
                ddgiVolume["origin"] >> vol.origin;
                ddgiVolume["rotation"] >> vol.eulerAngles;
                ddgiVolume["probeCounts"] >> vol.probeCounts;
                ddgiVolume["probeSpacing"] >> vol.probeSpacing;
                ddgiVolume["probeNumRays"] >> vol.probeNumRays;
                ddgiVolume["probeNumIrradianceTexels"] >> vol.probeNumIrradianceTexels;
                ddgiVolume["probeNumDistanceTexels"] >> vol.probeNumDistanceTexels;
                ddgiVolume["probeHysteresis"] >> vol.probeHysteresis;
                ddgiVolume["probeNormalBias"] >> vol.probeNormalBias;
                ddgiVolume["probeViewBias"] >> vol.probeViewBias;
                ddgiVolume["probeMaxRayDistance"] >> vol.probeMaxRayDistance;
                ddgiVolume["probeIrradianceThreshold"] >> vol.probeIrradianceThreshold;
                ddgiVolume["probeBrightnessThreshold"] >> vol.probeBrightnessThreshold;
                ddgiVolume["vis.probeVisType"] >> vol.vis.probeVisType;
                ddgiVolume["vis.probeRadius"] >> vol.vis.probeRadius;
                ddgiVolume["vis.probeDistanceDivisor"] >> vol.vis.probeDistanceDivisor;
                ddgiVolume["vis.showProbes"] >> vol.vis.showProbes;
                ddgiVolume["vis.texture.irradianceScale"] >>
                    vol.vis.texturesIrradianceScale;
                ddgiVolume["vis.texture.distanceScale"] >> vol.vis.texturesDistanceScale;
                ddgiVolume["vis.texture.probeDataScale"] >> vol.vis.texturesProbeDataScale;
                ddgiVolume["vis.texture.rayDataScale"] >> vol.vis.texturesRayDataScale;
                ddgiVolume["vis.texture.probeVariabilityScale"] >>
                    vol.vis.texturesVariabilityScale;

                config.renderers.ddgi.children.push_back(vol);
            }
        }
    }

    return true;
}

bool LoadConfigs(donut::vfs::IFileSystem* vfs, std::string_view filepath, Config& config) {
    std::filesystem::path configPath = filepath;
    std::filesystem::path relativePath = configPath.parent_path();

    Json::Value docRoot;
    if (!donut::json::LoadFromFile(*vfs, configPath, docRoot)) return false;

    if (!ParseConfigs(docRoot, relativePath, config)) return false;

    return true;
}

#include <cstdint>
#include <string>
#include <donut/core/math/math.h>
#include <vector>

namespace donut::vfs {
class IFileSystem;
};

struct Config {
    std::string configFileDir;

    struct Scene {
        std::string filePath;
        dm::float3 skyRadiance;
    } scene;

    struct Interaction {
        struct Mouse {
            float movementSpeed;
            float rotationSpeed;
            bool invertPan;
        } mouse;
    } interaction;

    struct Renderers {

        enum class RenderMode {
            PathTracing = 0,
            DDGI = 1
        } renderMode;

        struct PathTracing {
            float rayNormalBias;
            float rayViewBias;
            uint32_t numBounces;
            uint32_t samplersPerPixel;
            bool antialiasing;
        } pt;

        struct RTAO {
            bool enabled;
            float rayLength;
            float rayNormalBias;
            float rayViewBias;
            float powerLog;
            float filterDepthSigma;
            float filterDistanceSigma;
        } rtao;

        struct PostProcessing {
            struct Exposure {
                bool enabled;
                float fstops;
            } exposure;

            struct ToneMapping {
                bool enabled;
            } tonemapping;

            struct Dithering {
                bool enabled;
            } dithering;

            struct GammaCorrection {
                bool enabled;
            } gammaCorrection;
        } pp;

        struct DDGI {
            struct DDGIVolume {
                std::string name;
                bool probeRelocationEnabled;
                float probeRelocationMinFrontfaceDistance;
                bool probeClassificationEnabled;
                bool probeVariabilityEnabled;
                float probeVariabilityThreshold;
                bool infiniteScrollingEnabled;
                std::string texturesRayDataFormat;
                std::string texturesIrradianceFormat;
                std::string texturesDistanceFormat;
                std::string texturesDataFormat;
                std::string texturesVariabilityFormat;
                dm::float3 origin;
                dm::float3 eulerAngles;
                dm::uint3 probeCounts;
                dm::float3 probeSpacing;
                dm::uint probeNumRays;
                dm::uint probeNumIrradianceTexels;
                dm::uint probeNumDistanceTexels;
                float probeHysteresis;
                float probeNormalBias;
                float probeViewBias;
                float probeMaxRayDistance;
                float probeIrradianceThreshold;
                float probeBrightnessThreshold;
                struct Vis {
                    std::string probeVisType;
                    float probeRadius;
                    float probeDistanceDivisor;
                    bool showProbes;
                    float texturesIrradianceScale;
                    float texturesDistanceScale;
                    float texturesProbeDataScale;
                    float texturesRayDataScale;
                    float texturesVariabilityScale;
                } vis;
            };

            bool enabled;
            std::vector<DDGIVolume> children;
        } ddgi;
    } renderers;
};

bool LoadConfigs(donut::vfs::IFileSystem *vfs, std::string_view filepath, Config &config);

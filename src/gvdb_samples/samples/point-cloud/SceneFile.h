#ifndef POINT_CLOUD_SCENEFILE_H
#define POINT_CLOUD_SCENEFILE_H
// The scene file format of the reference gPointCloud sample (parse_scene /
// parse_value in main_point_cloud.cpp): a line that starts with a section
// keyword (points, polys, render, volume, camera, light, material, model)
// selects the section, every following "tag: value" line sets one of its
// fields. Vectors are written "<x, y, z>". Each "material" / "model" line
// starts a new entry.
#include <sample-utils/OptixRenderer.h>
#include <donut/core/math/math.h>
#include <filesystem>
#include <string>
#include <vector>

namespace PointCloud {

struct SceneModel {
    std::string path;     // directory prefix ("" = look in the assets)
    std::string file;
    int material = 0;
    float scale = 1.f;
    dm::float3 offset = dm::float3::zero();
};

struct SceneDesc {
    // points (a time series: path + file with a printf frame number)
    bool pointsOn = false;
    std::string pointPath;
    std::string pointFile;
    int pointMaterial = 0;
    int frame = -1;          // -1: not set
    int frameStep = 0;

    // polygon time series (optional)
    bool polysOn = false;
    std::string polyPath;
    std::string polyFile;
    int polyMaterial = 0;
    int polyFrame = 0;
    int polyFrameStep = 1;
    float polyScale = 1.f;
    dm::float3 polyOffset = dm::float3::zero();

    // render
    int width = 1280;
    int height = 760;
    int maxSamples = 1;
    dm::float4 backgroundColor = {0.1f, 0.2f, 0.4f, 1.f};
    std::string envmap;
    std::string outPath;
    std::string outFile = "img%04d.png";

    // volume (the reference defaults of Sample::init)
    float renderScale = 0.f;   // 0: not set (the scene file's "scale" applies)
    dm::float3 steps = {0.25f, 16.f, 0.25f};
    dm::float3 extinct = {-1.f, 1.1f, 0.f};
    dm::float3 range = {0.f, -1.f, 3.f};   // iso value, min, max
    dm::float3 cutoff = {0.005f, 0.001f, 0.f};
    int smooth = 0;
    dm::float3 smoothParams = dm::float3::zero();

    // camera / light orbits (target and distance in unscaled units; the
    // reference multiplies them by the render scale when it parses them)
    dm::float3 cameraAngles = {50.f, 30.f, 0.f};
    dm::float3 cameraTarget = {128.f, 128.f, 128.f};
    float cameraDistance = 1400.f;
    float cameraFov = 50.f;   // horizontal, degrees
    dm::float3 lightAngles = {0.f, 40.f, 0.f};
    dm::float3 lightTarget = {128.f, 128.f, 128.f};
    float lightDistance = 2000.f;

    std::vector<SampleUtils::OptixMaterialParams> materials;
    std::vector<SceneModel> models;
};

// Parses the file into desc (fields not mentioned keep their values). A
// render scale set in desc beforehand (the -scale argument) wins over the
// file's "volume / scale", as in the reference.
bool parseSceneFile(const std::filesystem::path &path, SceneDesc &desc);

// "pnt%04d.dat" + 12 -> "pnt0012.dat": the one printf conversion (%[0][width]d)
// the reference's file patterns use; a pattern without one is returned unchanged.
std::string formatFrameName(const std::string &pattern, int frame);

}  // namespace PointCloud

#endif /* POINT_CLOUD_SCENEFILE_H */

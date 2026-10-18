// SPDX-License-Identifier: Apache-2.0
#include "PointCloudInput.h"
#include "PointCloudScene.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
namespace {
void require(bool value, const char* what) {
    if (!value)
        throw std::runtime_error(what);
}
void expectNear(float actual, float expected, const char* what) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-5f * std::max(1.f, std::abs(expected)))
        throw std::runtime_error(what);
}
template <class F>
void rejects(F f, const char* what) {
    bool failed = false;
    try {
        f();
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, what);
}
void word(std::string& bytes, uint32_t value) {
    for (int i = 0; i < 4; ++i)
        bytes.push_back(char(value >> (8 * i)));
}
void scalar(std::string& bytes, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    word(bytes, bits);
}
std::string encoded() {
    std::string result;
    word(result, 3);
    for (float v : {-2.f, 4.f, 0.f, 6.f, 4.f, 8.f})
        scalar(result, v);
    for (uint32_t v : {0u, 0u, 0u, 32768u, 123u, 65535u, 65535u, 65535u, 32767u}) {
        result.push_back(char(v));
        result.push_back(char(v >> 8));
    }
    return result;
}
gvdb::samples::pointcloud::PointFrame parse(const std::string& bytes) {
    std::istringstream in(bytes, std::ios::binary);
    return gvdb::samples::pointcloud::readPointFrame(in);
}
}  // namespace
int main(int argc, char** argv) {
    try {
        using namespace gvdb::samples::pointcloud;
        require(argc == 2, "Usage: gvdb_point_cloud_input_tests asset-directory");
        const std::filesystem::path assets(argv[1]);
        const auto bytes = encoded();
        const auto frame = parse(bytes);
        require(frame.count() == 3 && frame.coordinates.size() == 9, "Odd-count tightly packed ushort3 input");
        const auto decoded = decodePointFrame(frame, 2);
        expectNear(decoded[0].x, -4, "Signed point decode");
        expectNear(decoded[1].y, 8, "Constant coordinate axis");
        expectNear(decoded[2].x, 12, "Maximum endpoint decode");
        expectNear(decoded[1].z, 16, "Packed 0xffff is normalized uint16, not IEEE half/NaN");
        require(parse(bytes + "extension").count() == 3, "Legacy loader accepts trailing bytes");
        for (size_t n = 0; n < 28; ++n)
            rejects([&] { parse(bytes.substr(0, n)); }, "Truncated point header accepted");
        for (size_t n = 28; n < bytes.size(); ++n)
            rejects([&] { parse(bytes.substr(0, n)); }, "Truncated or odd-byte payload accepted");
        for (uint32_t invalid : {0u, 0x80000000u, 0xffffffffu}) {
            auto malformed = bytes;
            for (int a = 0; a < 4; ++a)
                malformed[a] = char(invalid >> (a * 8));
            rejects([&] { parse(malformed); }, "Invalid signed count accepted");
        }
        for (uint32_t invalid : {0x7f800000u, 0xff800000u, 0x7fc00000u}) {
            auto malformed = bytes;
            for (int a = 0; a < 4; ++a)
                malformed[4 + a] = char(invalid >> (a * 8));
            rejects([&] { parse(malformed); }, "Nonfinite bounds accepted");
        }
        auto reversed = frame;
        reversed.maximum.x = -3;
        rejects([&] { validatePointFrame(reversed); }, "Reversed bounds accepted");
        rejects([&] { decodePointFrame(frame, 0); }, "Zero scale accepted");
        rejects([&] { decodePointFrame(frame, INFINITY); }, "Nonfinite scale accepted");
        auto odd = frame;
        odd.coordinates.pop_back();
        rejects([&] { validatePointFrame(odd); }, "Incomplete ushort3 accepted");
        const auto signedBounds = pointQuantizationBounds(decoded);
        expectNear(signedBounds.minimum.x, -5, "Signed padded lower bound");
        expectNear(signedBounds.range.x, 19, "Signed padded quantization extent");
        expectNear(signedBounds.range.y, 3, "Constant-axis quantization extent");
        const auto positive = pointQuantizationBounds({
            {.5f, 10, 20, 1},
            {2,   10, 24, 1}
        });
        expectNear(positive.minimum.x, 0, "Positive lower bounds retain clamp0");
        expectNear(positive.range.x, 4, "Positive quantization extent");
        auto badPoint = decoded;
        badPoint[0].z = NAN;
        rejects([&] { pointQuantizationBounds(badPoint); }, "Nonfinite point accepted");
        SceneDesc scene;
        require(parseSceneFile(assets / "teapot.scn", scene), "teapot.scn load");
        require(scene.pointsOn && !scene.polysOn && scene.pointFile == "points.dat" && scene.frame == 0 &&
                    scene.frameStep == 0,
                "Teapot point series fields");
        require(scene.models.size() == 3 && scene.materials.size() == 4 && scene.models[0].material == 3 &&
                    scene.models[1].file == "teapot.obj",
                "Teapot model/material schema");
        require(
            scene.width == 800 && scene.height == 600 && scene.maxSamples == 128 && scene.envmap == "sky.png",
            "Teapot render settings");
        expectNear(scene.renderScale, 2, "Teapot render scale");
        expectNear(scene.cameraAngles.x, 7, "Teapot camera angles");
        expectNear(scene.cameraTarget.z, 1084, "Teapot camera target");
        expectNear(scene.lightAngles.y, 128, "Teapot light angles");
        require(scene.smooth == 1, "Teapot smoothing iterations");
        expectNear(scene.smoothParams.y, -.2f, "Teapot smoothing bias");
        expectNear(scene.materials[0].refrIor, 1.33f, "Material refraction index");
        expectNear(scene.materials[0].refrAmount, .8f, "Material refraction amount");
        expectNear(scene.materials[0].refrOffset, 100, "Material refraction offset");
        expectNear(scene.materials[1].refrIor, 1.2f, "Material default retained");
        expectNear(scene.models[2].offset.x, 1056, "Model transform retained");
        SceneDesc overridden;
        overridden.renderScale = 3;
        require(parseSceneFile(assets / "teapot.scn", overridden), "Teapot override parse");
        expectNear(overridden.renderScale, 3, "CLI scale wins over file");
        SceneDesc flow;
        require(parseSceneFile(assets / "flow.scn", flow), "Flow scene parse");
        require(flow.pointPath == "d:/codes/gvdb_kui2/" && flow.pointFile == "pnt%04d.dat" &&
                    flow.frame == -1 && flow.frameStep == 0,
                "External series metadata/defaults retained");
        require(formatFrameName("pnt%04d.dat", 12) == "pnt0012.dat" && formatFrameName("%3d", 12) == " 12" &&
                    formatFrameName("%04d-%d", 12) == "0012-%d",
                "Frame pattern semantics");
        require(formatFrameName("%04d", -12) == "0-12" && formatFrameName("%x-%04d", 12) == "%x-%04d",
                "Legacy first-conversion and negative padding semantics");
        rejects([&] { formatFrameName("%999999999999999999d", 0); }, "Unbounded frame width accepted");
        require(formatFrameName("%999999999999999999x", 0) == "%999999999999999999x",
                "Unsupported formats remain unchanged");
        require(resolveInputPath(assets, "prefix", "name.dat") == std::filesystem::path("prefixname.dat"),
                "Literal prefix concatenation");
        require(resolveInputPath(assets, "", "points.dat") == assets / "points.dat", "Asset fallback path");
        const auto temp =
            std::filesystem::temp_directory_path() /
            ("gvdb-scene-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             ".scn");
        {
            std::ofstream out(temp);
            out << " camera\n dist: 123\npointsSuffix\n file: p%03d.dat\n frame: 3.9\n fstep: -2\npolys\n "
                   "path: literal/\n file: m%02d.obj\n mat: 2\n frame: 7\n fstep: 4\n scale: 99\nmaterial\n "
                   "diffuse: <1, 2>\n unknown: 99\n";
        }
        SceneDesc syntax;
        const bool parsed = parseSceneFile(temp, syntax);
        std::filesystem::remove(temp);
        require(parsed, "Scene syntax fixture parse");
        expectNear(syntax.cameraDistance, 1400, "Indented section must not change mode");
        require(syntax.pointsOn && syntax.frame == 3 && syntax.frameStep == -2 && syntax.polysOn &&
                    syntax.polyFrame == 7 && syntax.polyFrameStep == 4,
                "Raw prefix sections and truncated integer values");
        expectNear(syntax.polyScale, 1, "Legacy unknown polys scale is ignored");
        expectNear(syntax.materials[0].diffColor.z, 0, "Missing vector components default0");
        // Two actual packed files resolved through the original series prefix
        // and first printf conversion. A failed load must preserve the old frame.
        const std::string prefix = temp.string() + "-";
        const auto firstPath = resolveInputPath(assets, prefix, formatFrameName("%02d.dat", 0));
        const auto secondPath = resolveInputPath(assets, prefix, formatFrameName("%02d.dat", 2));
        auto onePoint = bytes.substr(0, 34);
        onePoint[0] = 1;
        {
            std::ofstream out(firstPath, std::ios::binary);
            out.write(bytes.data(), std::streamsize(bytes.size()));
        }
        {
            std::ofstream out(secondPath, std::ios::binary);
            out.write(onePoint.data(), std::streamsize(onePoint.size()));
        }
        auto firstFrame = loadPointFrame(firstPath);
        const auto secondFrame = loadPointFrame(secondPath);
        std::filesystem::remove(firstPath);
        std::filesystem::remove(secondPath);
        require(firstFrame.count() == 3 && secondFrame.count() == 1,
                "Two packed time-series frames retain independent counts");
        rejects([&] { firstFrame = loadPointFrame(secondPath); }, "Missing time-series frame accepted");
        require(firstFrame.count() == 3, "Failed frame load preserves existing points");
        const auto real = loadPointFrame(assets / "points.dat");
        require(real.count() == 1867648, "Real point asset count");
        expectNear(real.minimum.x, 711.9174194335938f, "Real point minimum");
        expectNear(real.maximum.z, 1330.4871826171875f, "Real point maximum");
        const auto realPositions = decodePointFrame(real, scene.renderScale);
        float minimum = INFINITY, maximum = -INFINITY;
        for (const auto& p : realPositions) {
            require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z),
                    "Real decoded finite coordinates");
            minimum = std::min(minimum, p.y);
            maximum = std::max(maximum, p.y);
        }
        expectNear(minimum, real.minimum.y * 2, "Real y minimum endpoint");
        uint16_t maxY = 0, minY = 65535;
        for (size_t i = 1; i < real.coordinates.size(); i += 3) {
            maxY = std::max(maxY, real.coordinates[i]);
            minY = std::min(minY, real.coordinates[i]);
        }
        expectNear(minimum, (real.minimum.y + float(minY) * (real.maximum.y - real.minimum.y) / 65535.f) * 2,
                   "Real occupied minimum");
        expectNear(maximum, (real.minimum.y + float(maxY) * (real.maximum.y - real.minimum.y) / 65535.f) * 2,
                   "Real occupied maximum");
        std::cout << "Point-cloud CPU input/scene fixtures PASS: " << real.count()
                  << " real ushort3 points, teapot materials/models preserved\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Point-cloud input fixtures: " << e.what() << '\n';
        return 1;
    }
}

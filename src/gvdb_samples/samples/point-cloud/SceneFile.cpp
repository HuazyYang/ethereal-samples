// Scene file parser of the point-cloud sample (see SceneFile.h).
#include "SceneFile.h"
#include <donut/core/log.h>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace PointCloud {

namespace {

enum Section { S_GLOBAL, S_RENDER, S_LIGHT, S_CAMERA, S_MODEL, S_POINTS, S_POLYS, S_VOLUME, S_MATERIAL };

std::string trim(const std::string &s) {
    const char *ws = " \t\r\n";
    size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

bool startsWith(const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; }

float toNum(const std::string &s) { return float(atof(s.c_str())); }

// "<x, y, z>" (the reference strToVec3 with "<", ",", ">"); missing components stay 0.
dm::float3 toVec3(const std::string &s) {
    dm::float3 v = dm::float3::zero();
    size_t b = s.find('<'), e = s.find('>');
    std::string body = (b != std::string::npos && e != std::string::npos && e > b) ? s.substr(b + 1, e - b - 1) : s;
    std::stringstream ss(body);
    std::string item;
    for (int i = 0; i < 3 && std::getline(ss, item, ','); ++i) (&v.x)[i] = toNum(item);
    return v;
}

void parseValue(SceneDesc &d, Section mode, const std::string &tag, const std::string &val) {
    switch (mode) {
        case S_POINTS:
            if (tag == "path") d.pointPath = val;
            if (tag == "file") d.pointFile = val;
            if (tag == "mat") d.pointMaterial = int(toNum(val));
            if (tag == "frame") d.frame = int(toNum(val));
            if (tag == "fstep") d.frameStep = int(toNum(val));
            break;
        case S_POLYS:
            if (tag == "path") d.polyPath = val;
            if (tag == "file") d.polyFile = val;
            if (tag == "mat") d.polyMaterial = int(toNum(val));
            if (tag == "frame") d.polyFrame = int(toNum(val));
            if (tag == "fstep") d.polyFrameStep = int(toNum(val));
            break;
        case S_MATERIAL: {
            if (d.materials.empty()) break;
            SampleUtils::OptixMaterialParams &m = d.materials.back();
            if (tag == "lightwid") m.lightWidth = toNum(val);
            if (tag == "shwid") m.shadowWidth = toNum(val);
            if (tag == "shbias") m.shadowBias = toNum(val);
            if (tag == "ambient") m.ambColor = toVec3(val);
            if (tag == "diffuse") m.diffColor = toVec3(val);
            if (tag == "spec") m.specColor = toVec3(val);
            if (tag == "spow") m.specPower = toNum(val);
            if (tag == "env") m.envColor = toVec3(val);
            if (tag == "reflwid") m.reflWidth = toNum(val);
            if (tag == "reflbias") m.reflBias = toNum(val);
            if (tag == "reflcolor") m.reflColor = toVec3(val);
            if (tag == "refrwid") m.refrWidth = toNum(val);
            if (tag == "refrbias") m.refrBias = toNum(val);
            if (tag == "refrcolor") m.refrColor = toVec3(val);
            if (tag == "refroffs") m.refrOffset = toNum(val);
            if (tag == "refrior") m.refrIor = toNum(val);
            if (tag == "reframt") m.refrAmount = toNum(val);
        } break;
        case S_RENDER:
            if (tag == "width") d.width = int(toNum(val));
            if (tag == "height") d.height = int(toNum(val));
            if (tag == "samples") d.maxSamples = int(toNum(val));
            if (tag == "backclr") {
                dm::float3 c = toVec3(val);
                d.backgroundColor = {c.x, c.y, c.z, 1.f};
            }
            if (tag == "envmap") d.envmap = val;
            if (tag == "outpath") d.outPath = val;
            if (tag == "outfile") d.outFile = val;
            break;
        case S_VOLUME:
            if (tag == "scale" && d.renderScale == 0.f) d.renderScale = toNum(val);
            if (tag == "steps") d.steps = toVec3(val);
            if (tag == "extinct") d.extinct = toVec3(val);
            if (tag == "range") d.range = toVec3(val);
            if (tag == "cutoff") d.cutoff = toVec3(val);
            if (tag == "smooth") d.smooth = int(toNum(val));
            if (tag == "smoothp") d.smoothParams = toVec3(val);
            break;
        case S_CAMERA:
            if (tag == "angs") d.cameraAngles = toVec3(val);
            if (tag == "target") d.cameraTarget = toVec3(val);
            if (tag == "dist") d.cameraDistance = toNum(val);
            if (tag == "fov") d.cameraFov = toNum(val);
            break;
        case S_LIGHT:
            if (tag == "angs") d.lightAngles = toVec3(val);
            if (tag == "target") d.lightTarget = toVec3(val);
            if (tag == "dist") d.lightDistance = toNum(val);
            break;
        case S_MODEL: {
            if (d.models.empty()) break;
            SceneModel &m = d.models.back();
            if (tag == "path") m.path = val;
            if (tag == "file") m.file = val;
            if (tag == "mat") m.material = int(toNum(val));
            if (tag == "scale") m.scale = toNum(val);
            if (tag == "offset") m.offset = toVec3(val);
        } break;
        default:
            break;
    }
}

}  // namespace

bool parseSceneFile(const std::filesystem::path &path, SceneDesc &desc) {
    std::ifstream file(path);
    if (!file) {
        donut::log::error("Cannot find scene file %s", path.string().c_str());
        return false;
    }
    Section mode = S_GLOBAL;
    std::string line;
    while (std::getline(file, line)) {
        // section keywords at the start of the line (the reference tests the raw line)
        if (startsWith(line, "points")) { desc.pointsOn = true; mode = S_POINTS; }
        if (startsWith(line, "polys")) { desc.polysOn = true; mode = S_POLYS; }
        if (startsWith(line, "light")) mode = S_LIGHT;
        if (startsWith(line, "camera")) mode = S_CAMERA;
        if (startsWith(line, "global")) mode = S_GLOBAL;
        if (startsWith(line, "render")) mode = S_RENDER;
        if (startsWith(line, "volume")) mode = S_VOLUME;
        if (startsWith(line, "material")) { mode = S_MATERIAL; desc.materials.emplace_back(); }
        if (startsWith(line, "model")) { mode = S_MODEL; desc.models.emplace_back(); }

        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string tag = trim(line.substr(0, colon));
        std::string val = trim(line.substr(colon + 1));
        if (!tag.empty() && !val.empty()) parseValue(desc, mode, tag, val);
    }
    return true;
}

std::string formatFrameName(const std::string &pattern, int frame) {
    size_t p = pattern.find('%');
    if (p == std::string::npos) return pattern;
    size_t i = p + 1;
    bool zero = false;
    if (i < pattern.size() && pattern[i] == '0') { zero = true; ++i; }
    int width = 0;
    while (i < pattern.size() && isdigit(uint8_t(pattern[i]))) width = width * 10 + (pattern[i++] - '0');
    if (i >= pattern.size() || pattern[i] != 'd') return pattern;
    std::ostringstream out;
    out << pattern.substr(0, p);
    if (zero) out << std::setfill('0');
    out << std::setw(width) << frame << pattern.substr(i + 1);
    return out.str();
}

}  // namespace PointCloud

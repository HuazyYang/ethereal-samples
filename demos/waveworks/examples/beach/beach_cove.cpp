#include "beach_cove.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
float Gaussian(float x) {
    return std::exp(-x * x);
}
using Point = BeachCoastPoint;
Point operator+(Point a, Point b) {
    return {a.x + b.x, a.z + b.z};
}
Point operator-(Point a, Point b) {
    return {a.x - b.x, a.z - b.z};
}
Point operator*(Point a, float s) {
    return {a.x * s, a.z * s};
}
float Dot(Point a, Point b) {
    return a.x * b.x + a.z * b.z;
}
float Length(Point a) {
    return std::sqrt(Dot(a, a));
}

// Authored world-space geometry, metres. The near sand tongue turns back on
// itself in X; the far spit wraps into a small lee pocket before the coast
// retreats inland. No camera coordinates or output-color controls at runtime.
constexpr Point CoastControls[] = {
    {-120, -600}, {-85, -260}, {-62, -150}, {-49, -93}, {-46, -80}, {-43, -68}, {-35, -60},
    {-32, -50},   {-38, -40},  {-43, -27},  {-42, -16}, {-38, -5},  {-29, 6},   {-12, 16},
    {11, 22},     {39, 12},    {65, -9},    {97, -37},  {112, -42}, {136, -29}, {156, -5},
    {156, 19},    {165, 40},   {207, 59},   {235, 119}, {270, 248}, {242, 398}, {225, 650}};

// Barry-Goldman evaluation of centripetal Catmull-Rom (alpha=1/2).
// Curve interpolation authors GEOMETRY only; shading remains the shared PBR path.
Point CurvePoint(Point p0, Point p1, Point p2, Point p3, float u) {
    float t0 = 0, t1 = std::sqrt(Length(p1 - p0));
    float t2 = t1 + std::sqrt(Length(p2 - p1));
    float t3 = t2 + std::sqrt(Length(p3 - p2));
    float t = t1 + u * (t2 - t1);
    auto blend = [t](Point a, Point b, float ta, float tb) {
        return a * ((tb - t) / (tb - ta)) + b * ((t - ta) / (tb - ta));
    };
    Point a1 = blend(p0, p1, t0, t1), a2 = blend(p1, p2, t1, t2);
    Point a3 = blend(p2, p3, t2, t3);
    return blend(blend(a1, a2, t0, t2), blend(a2, a3, t1, t3), t1, t2);
}

class CoastGeometry {
    struct Node {
        Point minimum, maximum;
        uint32_t begin, end, left = 0, right = 0;
    };
    std::vector<Node> nodes;

    uint32_t BuildNode(uint32_t begin, uint32_t end) {
        Node node{points[begin], points[begin], begin, end};
        for (uint32_t i = begin + 1; i <= end; ++i) {
            node.minimum.x = std::min(node.minimum.x, points[i].x);
            node.minimum.z = std::min(node.minimum.z, points[i].z);
            node.maximum.x = std::max(node.maximum.x, points[i].x);
            node.maximum.z = std::max(node.maximum.z, points[i].z);
        }
        uint32_t index = uint32_t(nodes.size());
        nodes.push_back(node);
        if (end - begin > 8) {
            uint32_t middle = begin + (end - begin) / 2;
            uint32_t left = BuildNode(begin, middle), right = BuildNode(middle, end);
            nodes[index].left = left;
            nodes[index].right = right;
        }
        return index;
    }

    float BoxDistance(Point p, uint32_t index) const {
        const auto& n = nodes[index];
        float dx = std::max({n.minimum.x - p.x, 0.f, p.x - n.maximum.x});
        float dz = std::max({n.minimum.z - p.z, 0.f, p.z - n.maximum.z});
        return dx * dx + dz * dz;
    }

    void Nearest(Point p, uint32_t index, float& best) const {
        if (BoxDistance(p, index) > best)
            return;
        const auto& n = nodes[index];
        if (n.left) {
            uint32_t first = n.left, second = n.right;
            if (BoxDistance(p, first) > BoxDistance(p, second))
                std::swap(first, second);
            Nearest(p, first, best);
            Nearest(p, second, best);
        } else {
            for (uint32_t i = n.begin; i < n.end; ++i) {
                Point a = points[i], ab = points[i + 1] - a;
                float t = std::clamp(Dot(p - a, ab) / Dot(ab, ab), 0.f, 1.f);
                Point delta = p - (a + ab * t);
                best = std::min(best, Dot(delta, delta));
            }
        }
    }

    uint32_t Crossings(Point p, uint32_t index) const {
        const auto& n = nodes[index];
        if (p.z < n.minimum.z || p.z >= n.maximum.z || p.x >= n.maximum.x)
            return 0;
        if (n.left)
            return Crossings(p, n.left) + Crossings(p, n.right);
        uint32_t count = 0;
        for (uint32_t i = n.begin; i < n.end; ++i) {
            Point a = points[i], b = points[i + 1];
            if ((a.z > p.z) != (b.z > p.z))
                count += p.x < a.x + (p.z - a.z) * (b.x - a.x) / (b.z - a.z);
        }
        return count;
    }

 public:
    std::vector<Point> points;
    CoastGeometry() {
        constexpr size_t count = sizeof(CoastControls) / sizeof(CoastControls[0]);
        for (size_t i = 0; i + 1 < count; ++i) {
            Point p1 = CoastControls[i], p2 = CoastControls[i + 1];
            Point p0 = i ? CoastControls[i - 1] : p1 * 2.f - p2;
            Point p3 = i + 2 < count ? CoastControls[i + 2] : p2 * 2.f - p1;
            uint32_t steps = uint32_t(std::ceil(Length(p2 - p1) / 0.35f));
            for (uint32_t j = 0; j < steps; ++j)
                points.push_back(CurvePoint(p0, p1, p2, p3, float(j) / steps));
        }
        points.push_back(CoastControls[count - 1]);
        BuildNode(0, uint32_t(points.size() - 1));
    }
    float SignedDistance(Point p) const {
        float best = std::numeric_limits<float>::max();
        Nearest(p, 0, best);
        // Ray parity handles the re-entrant sand tongues. Choosing the side
        // of just the nearest segment would misclassify concave corners.
        return std::sqrt(best) * (Crossings(p, 0) % 2 ? 1.f : -1.f);
    }
};

const CoastGeometry& Coast() {
    static const CoastGeometry coast;
    return coast;
}

float Ramp(float low, float high, float x) {
    float t = std::clamp((x - low) / (high - low), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

float UplandCoverage(float x, float z) {
    return Ramp(-35.f, 0.f, x) * (1.f - Ramp(205.f, 285.f, x)) *
           (1.f - Ramp(55.f, 100.f, x) * (1.f - Ramp(0.f, 65.f, z)));
}
float DetailElevation(float x, float z, float distance, const BeachGroundTextures& ground) {
    if (distance <= 0)
        return 0;
    float upland = UplandCoverage(x, z) * Ramp(12.f, 27.f, distance);
    // Photogrammetric height samples, using the same registered stochastic UV
    // transforms as the material. Amplitudes are art-authored metres, not a
    // claim that the reference image contains measurable height information.
    float relief = upland * BeachGroundReliefScale *
                   (ground.groundHeight.SampleStochastic(x / BeachGroundScanSize,
                                                         z / BeachGroundScanSize) -
                    .5f);
    float sand =
        Ramp(4.f, 12.f, distance) * (1 - upland) * .10f *
        (ground.sandHeight.SampleStochastic(x / BeachSandScanSize, z / BeachSandScanSize) -
         .5f);
    return relief + sand;
}
}  // namespace

const std::vector<BeachCoastPoint>& BeachCoveCoastline() {
    return Coast().points;
}
float BeachCoveSignedDistance(float x, float z) {
    return Coast().SignedDistance({x, z});
}
float BeachCoveDetailElevation(float x, float z, const BeachGroundTextures& ground) {
    return DetailElevation(x, z, BeachCoveSignedDistance(x, z), ground);
}

float BeachCoveElevation(float x, float z) {
    float signedDistance = BeachCoveSignedDistance(x, z);
    float d = std::abs(signedDistance);
    if (signedDistance < 0) {
        // Continuous sandy shoreface, submerged bar and outer shelf. No
        // rescaled survey trench: h and dh/dd remain bounded at the coast.
        float bar = 0.42f * Gaussian((d - 23.f) / 9.f) * (1.f - std::exp(-d / 4.f));
        // Concave shoreface: a gentle swash slope deepens into the cove's
        // subtidal basin. Its slope tends to 0.41 rather than growing without
        // bound. Optical water depth comes from this mesh, not a color ramp.
        float shoreface = 0.13f * d + 0.28f * (d - 14.f * (1.f - std::exp(-d / 14.f)));
        return -shoreface + bar;
    }
    // Broad sandy foreshore and low foreground tongue; the upland sits behind
    // the recessed bay, not along an infinite repeating coastal escarpment.
    float upland = Ramp(-35.f, 0.f, x) * (1.f - Ramp(205.f, 285.f, x));
    // The far lobe is a low sand spit with a small dune, not a cliff jutting
    // into the bay. High ground remains inland of its neck.
    upland *= 1.f - Ramp(55.f, 100.f, x) * (1.f - Ramp(0.f, 65.f, z));
    float bluffDistance = std::max(d - 18.f - 2.f * std::sin(x * 0.037f), 0.f);
    float bluff = upland * (12.f + 14.f * Ramp(0.f, 160.f, x)) *
                  (1.f - std::exp(-bluffDistance / 26.f));
    float landRamp = 1.f - std::exp(-d / 4.f);
    float dune =
        0.38f * Gaussian((d - 14.f) / 8.f) * landRamp * (1.f + 0.15f * std::sin(x * 0.08f));
    float spitKnoll =
        1.55f * Gaussian((x - 124.f) / 12.f) * Gaussian((z + 15.f) / 9.f) * landRamp;
    return 2.5f * (1.f - std::exp(-d / 28.f)) + bluff + dune + spitKnoll;
}

bool BuildBeachCove(const BeachCoveSettings& s, std::vector<BeachVertex>& vertices,
                    std::vector<uint32_t>& indices) {
    vertices.clear();
    indices.clear();
    if (!s.ground || !s.ground->IsValid() || s.width < 2 || s.height < 2 ||
        s.width > 2049 || s.height > 2049 || !std::isfinite(s.minimumX) ||
        !std::isfinite(s.minimumZ) || !std::isfinite(s.extentX) ||
        !std::isfinite(s.extentZ) || !std::isfinite(s.meanSeaLevel) || s.extentX <= 0 ||
        s.extentZ <= 0)
        return false;
    float dx = s.extentX / float(s.width - 1), dz = s.extentZ / float(s.height - 1);
    vertices.resize(size_t(s.width) * s.height);
    indices.reserve(size_t(s.width - 1) * (s.height - 1) * 6);
    for (uint32_t j = 0; j < s.height; ++j)
        for (uint32_t i = 0; i < s.width; ++i) {
            float x = s.minimumX + i * dx, z = s.minimumZ + j * dz;
            vertices[size_t(j) * s.width + i] = {
                x,
                BeachCoveElevation(x, z) + BeachCoveDetailElevation(x, z, *s.ground) +
                    s.meanSeaLevel,
                z,
                0,
                1,
                0,
                x * 0.12f,
                z * 0.12f};  // Metre-based rock UVs, survey UVs unchanged.
        }
    for (uint32_t j = 0; j < s.height; ++j)
        for (uint32_t i = 0; i < s.width; ++i) {
            uint32_t left = i ? i - 1 : i, right = std::min(i + 1, s.width - 1);
            uint32_t down = j ? j - 1 : j, up = std::min(j + 1, s.height - 1);
            float nx = -(vertices[size_t(j) * s.width + right].y -
                         vertices[size_t(j) * s.width + left].y) /
                       ((right - left) * dx);
            float nz = -(vertices[size_t(up) * s.width + i].y -
                         vertices[size_t(down) * s.width + i].y) /
                       ((up - down) * dz);
            float inverseLength = 1.f / std::sqrt(1.f + nx * nx + nz * nz);
            auto& v = vertices[size_t(j) * s.width + i];
            v.nx = nx * inverseLength;
            v.ny = inverseLength;
            v.nz = nz * inverseLength;
            if (i + 1 < s.width && j + 1 < s.height) {
                uint32_t a = j * s.width + i, b = a + 1, c = a + s.width, d = c + 1;
                indices.insert(indices.end(), {a, c, b, b, c, d});
            }
        }
    return true;
}

void ClassifyBeachCoveGround(float seaLevel, const BeachGroundTextures& ground,
                             std::vector<BeachTerrainRenderVertex>& vertices) {
    for (auto& v : vertices) {
        float height = v.y - seaLevel;
        float coverTexture = ground.cover.SampleStochastic(v.x / BeachGroundScanSize,
                                                           v.z / BeachGroundScanSize);
        float beachLimit = 2.30f + .20f * (coverTexture - .5f);
        uint32_t cover = 0;
        if (height < beachLimit)
            cover = 0x000000ffu;
        else {
            // Classify photographed green cover into grass and exposed soil.
            // A narrower coverage interval preserves distinct patches instead
            // of mixing both materials over almost the entire upland.
            const float grassFraction =
                v.ny > 0.72f ? std::clamp((coverTexture - .15f) * 4.f, 0.f, 1.f) : 0.f;
            const uint32_t grass = uint32_t(std::lround(grassFraction * 255.f));
            cover = (grass << 8) | ((255 - grass) << 16);
        }
        if (height <= 0.22f && (cover & 0xffu))
            cover |= 0xff000000u;
        v.packedMaterialWeights = cover;
    }
}

BeachShallowSettings BeachCoveShallowSettings(float seaLevel) {
    BeachShallowSettings settings;
    settings.width = 320;
    settings.height = 256;
    settings.originX = -92.f;
    settings.originZ = -116.f;
    settings.cellSizeX = settings.cellSizeZ = 1.f;
    settings.seaLevel = seaLevel;
    // A 0.70 m incident wave height on the cove's gently sloping shelf.
    // This drives the conservation equations, not a shoreline shading mask.
    settings.swellAmplitude = 0.35f;
    return settings;
}

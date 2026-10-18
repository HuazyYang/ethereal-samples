// SPDX-License-Identifier: Apache-2.0
#include "VolumeViewer.h"
#include "OwnedVolumeView.h"
#include "VolumeSurface.h"
#include <donut/app/DeviceManager.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/ImGuiRenderPass.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/core/vfs/VFS.h>
#include <donut/core/math/math.h>
#include <donut/core/log.h>
#include <GLFW/glfw3.h>
#include <stb_image_write.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
using namespace donut;
namespace gvdb::sample {
namespace g = gvdb::slang;
namespace es = ethereal::slang;
slang::SlangVolume& VolumeScene::volume() {
    throw std::logic_error("This scene does not provide a legacy volume renderer");
}
const slang::PackedVolume& VolumeScene::packed() const {
    throw std::logic_error("This scene does not provide legacy packed topology");
}
namespace {
void check(nvrhi::FRESULT result, const char* what) {
    if (NVRHI_FAILED(result))
        throw std::runtime_error(std::string(what) + " failed: " + std::to_string(result));
}
g::Float4 f4(dm::float3 v, float w = 0) { return {v.x, v.y, v.z, w}; }
struct ViewSettings {
    g::UInt4 rectangle, lineGrid;
    g::Float4 section;
    g::UInt4 flags;
};
struct Line {
    g::Float4 points, color;
};
struct Camera {
    dm::float3 eye, right, up, forward;
    float horizontal, vertical, radius;
};
Camera cameraFor(const g::VolumeInfo& info, const VolumeViewOptions& o) {
    const dm::float3 low(info.boundsMin.x, info.boundsMin.y, info.boundsMin.z),
        high(info.boundsMax.x, info.boundsMax.y, info.boundsMax.z);
    const auto back =
        dm::float3(std::sin(o.yaw) * std::cos(o.pitch), std::sin(o.pitch), std::cos(o.yaw) * std::cos(o.pitch));
    Camera c;
    c.radius = std::max(1.f, dm::length(high - low) * .5f);
    c.eye = (low + high) * .5f + dm::float3(o.targetOffset.x, o.targetOffset.y, o.targetOffset.z) +
            back * c.radius * o.distanceScale;
    c.forward = -back;
    if (o.explicitCamera) {
        c.eye = {o.cameraEye.x, o.cameraEye.y, o.cameraEye.z};
        c.forward = dm::normalize(dm::float3(o.cameraForward.x, o.cameraForward.y, o.cameraForward.z));
        c.right = dm::normalize(dm::cross(c.forward, dm::float3(o.cameraUp.x, o.cameraUp.y, o.cameraUp.z)));
        c.up = dm::cross(c.right, c.forward);
        c.horizontal = std::tan(dm::radians(o.horizontalFov * .5f));
        c.vertical = c.horizontal * float(o.height) / o.width;
        return c;
    }
    c.right = dm::normalize(dm::cross(c.forward, dm::float3(0, 1, 0)));
    c.up = dm::cross(c.right, c.forward);
    c.horizontal = std::tan(dm::radians(25.f));
    c.vertical = c.horizontal * float(o.height) / o.width;
    return c;
}
g::RaycastParameters parameters(const g::VolumeInfo& info, const VolumeViewOptions& o, const Camera& c,
                                size_t transferSize) {
    g::RaycastParameters p{};
    p.volume = info;
    const double steps = o.mode == VolumeViewMode::Section2D ? 1
                         : o.mode == VolumeViewMode::Voxel   ? double(info.boundsMax.x - info.boundsMin.x) +
                                                                 double(info.boundsMax.y - info.boundsMin.y) +
                                                                 double(info.boundsMax.z - info.boundsMin.z) + 4
                                                           : std::ceil(double(c.radius) * 2 / o.step) + 16;
    if (steps > 1000000)
        throw std::invalid_argument("Volume viewer ray step requires more than one million samples");
    p.image = {o.width, o.height, uint32_t(transferSize), uint32_t(steps)};
    p.rayOrigin = f4(c.eye);
    p.direction00 = f4(c.forward - c.right * c.horizontal + c.up * c.vertical, o.explicitCamera ? 1.f : 0.f);
    p.directionDx = f4(c.right * (2 * c.horizontal / o.width));
    p.directionDy = f4(c.up * (-2 * c.vertical / o.height));
    p.march = {o.step, o.explicitCamera ? o.cameraNear : 0,
               o.explicitCamera ? o.cameraFar : c.radius * (o.distanceScale + 4), o.extinction};
    p.valueRange = {o.valueMin, o.valueMax, o.isovalue, .995f};
    p.background = o.background;
    const auto center =
        o.explicitCamera ? dm::float3(info.boundsMin.x + info.boundsMax.x, info.boundsMin.y + info.boundsMax.y,
                                      info.boundsMin.z + info.boundsMax.z) *
                               .5f
                         : c.eye - c.radius * o.distanceScale * (-c.forward);
    p.light =
        f4(center + dm::float3(o.lightDirection.x, o.lightDirection.y, o.lightDirection.z) * c.radius, .15f);
    return p;
}
nvrhi::BufferHandle settingsBuffer(es::ComputeRuntime& runtime, const ViewSettings& settings) {
    nvrhi::BufferDesc d;
    d.byteSize = 256;
    d.isConstantBuffer = true;
    d.initialState = nvrhi::ResourceStates::ConstantBuffer;
    d.keepInitialState = true;
    nvrhi::BufferHandle buffer;
    check(runtime.device()->createBuffer(d, &buffer), "create viewer constants");
    uint8_t bytes[256]{};
    std::memcpy(bytes, &settings, sizeof(settings));
    runtime.upload(buffer, bytes, sizeof(bytes));
    return buffer;
}
bool clipScreenLine(dm::double2 p, dm::double2 q, const VolumeViewOptions& o, Line& line) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(q.x) || !std::isfinite(q.y))
        throw std::invalid_argument("Viewer overlay projection exceeds finite coordinates");
    const dm::double2 delta = q - p;
    double lo = 0, hi = 1;
    const double origin[2] = {p.x, p.y}, direction[2] = {delta.x, delta.y},
                 limit[2] = {double(o.width), double(o.height)};
    for (unsigned axis = 0; axis < 2; ++axis) {
        if (direction[axis] == 0) {
            if (origin[axis] < 0 || origin[axis] > limit[axis])
                return false;
        } else {
            double x = (0 - origin[axis]) / direction[axis], y = (limit[axis] - origin[axis]) / direction[axis];
            if (x > y)
                std::swap(x, y);
            lo = std::max(lo, x);
            hi = std::min(hi, y);
        }
    }
    if (lo > hi)
        return false;
    q = p + delta * hi;
    p += delta * lo;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(q.x) || !std::isfinite(q.y))
        throw std::invalid_argument("Nonfinite clipped overlay");
    // Clamp the final double-precision intersection before integer tile conversion.
    line.points = {float(std::clamp(p.x, 0., double(o.width))), float(std::clamp(p.y, 0., double(o.height))),
                   float(std::clamp(q.x, 0., double(o.width))), float(std::clamp(q.y, 0., double(o.height)))};
    return true;
}
bool projectLine(dm::float3 start, dm::float3 end, const Camera& c, const VolumeViewOptions& o, Line& line) {
    const auto toView = [&](dm::float3 v) {
        const dm::double3 d(double(v.x) - c.eye.x, double(v.y) - c.eye.y, double(v.z) - c.eye.z);
        const auto dot = [&](dm::float3 axis) { return d.x * axis.x + d.y * axis.y + d.z * axis.z; };
        return dm::double3(dot(c.right), dot(c.up), dot(c.forward));
    };
    auto a = toView(start), b = toView(end);
    const double nearPlane = o.explicitCamera ? o.cameraNear : std::max(.001f, c.radius * .001f);
    if (a.z < nearPlane && b.z < nearPlane)
        return false;
    if (a.z < nearPlane)
        a += (b - a) * ((nearPlane - a.z) / (b.z - a.z));
    if (b.z < nearPlane)
        b += (a - b) * ((nearPlane - b.z) / (a.z - b.z));
    if (o.explicitCamera) {
        const double farPlane = o.cameraFar;
        if (a.z > farPlane && b.z > farPlane)
            return false;
        if (a.z > farPlane)
            a += (b - a) * ((farPlane - a.z) / (b.z - a.z));
        if (b.z > farPlane)
            b += (a - b) * ((farPlane - b.z) / (a.z - b.z));
    }
    const dm::double2 p((a.x / (a.z * c.horizontal) + 1) * o.width * .5,
                        (1 - a.y / (a.z * c.vertical)) * o.height * .5);
    const dm::double2 q((b.x / (b.z * c.horizontal) + 1) * o.width * .5,
                        (1 - b.y / (b.z * c.vertical)) * o.height * .5);
    return clipScreenLine(p, q, o, line);
}
bool projectSectionLine(const g::Float4& a, const g::Float4& b, const g::VolumeInfo& info,
                        const VolumeViewOptions& o, Line& line) {
    const double sx = double(o.width) / (double(info.boundsMax.x) - info.boundsMin.x),
                 sy = double(o.height) / (double(info.boundsMax.z) - info.boundsMin.z);
    return clipScreenLine({(double(a.x) - info.boundsMin.x) * sx, (double(a.z) - info.boundsMin.z) * sy},
                          {(double(b.x) - info.boundsMin.x) * sx, (double(b.z) - info.boundsMin.z) * sy}, o,
                          line);
}
void drawOverlayLines(es::ComputeRuntime& runtime, const std::vector<Line>& lines, const VolumeViewOptions& o,
                      const g::RaycastParameters& p, ViewSettings settings,
                      std::vector<nvrhi::BindingSetItem> bindings) {
    if (lines.empty())
        return;
    constexpr uint32_t tile = 16;
    const uint32_t nx = (o.width + tile - 1) / tile, ny = (o.height + tile - 1) / tile;
    std::vector<std::vector<uint32_t>> bins(size_t(nx) * ny);
    size_t references = 0;
    constexpr size_t maxReferences = 16 * 1024 * 1024;
    for (uint32_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const auto& line = lines[lineIndex];
        const int x0 = std::max(0, int(std::floor((std::min(line.points.x, line.points.z) - 1) / tile)));
        const int y0 = std::max(0, int(std::floor((std::min(line.points.y, line.points.w) - 1) / tile)));
        const int x1 =
            std::min(int(nx) - 1, int(std::floor((std::max(line.points.x, line.points.z) + 1) / tile)));
        const int y1 =
            std::min(int(ny) - 1, int(std::floor((std::max(line.points.y, line.points.w) + 1) / tile)));
        const size_t added = x1 >= x0 && y1 >= y0 ? size_t(x1 - x0 + 1) * size_t(y1 - y0 + 1) : 0;
        if (added > maxReferences - references)
            throw std::runtime_error("Viewer overlay exceeds its 16-million tile-reference budget");
        references += added;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                bins[size_t(y) * nx + x].push_back(lineIndex);
    }
    std::vector<g::UInt2> ranges(bins.size());
    std::vector<uint32_t> indices;
    for (size_t i = 0; i < bins.size(); ++i) {
        if (indices.size() + bins[i].size() > UINT32_MAX / 4)
            throw std::runtime_error("Viewer overlay exceeds portable storage");
        ranges[i] = {uint32_t(indices.size()), uint32_t(bins[i].size())};
        indices.insert(indices.end(), bins[i].begin(), bins[i].end());
    }
    if (indices.empty())
        return;
    auto lineBuffer = runtime.buffer(lines.size() * sizeof(Line), sizeof(Line), lines.data());
    auto rangeBuffer = runtime.buffer(ranges.size() * sizeof(g::UInt2), sizeof(g::UInt2), ranges.data());
    auto indexBuffer = runtime.buffer(indices.size() * 4, 4, indices.data());
    settings.lineGrid = {tile, nx, ny, 0};
    auto constants = settingsBuffer(runtime, settings);
    bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(1, constants));
    bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_SRV(5, lineBuffer));
    bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, rangeBuffer));
    bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, indexBuffer));
    runtime.dispatch("VolumeView", "overlayTopology", &p, sizeof(p), bindings, (o.width + 7) / 8,
                     (o.height + 7) / 8);
}
void addTopology(es::ComputeRuntime& runtime, const g::PackedVolume& packed, const Camera& camera,
                 const VolumeViewOptions& o, const g::RaycastParameters& p, ViewSettings settings,
                 std::vector<nvrhi::BindingSetItem> bindings) {
    std::vector<Line> lines;
    const g::Float4 colors[] = {
        {.1f, 1,   .2f, 1},
        {1,   .7f, .1f, 1},
        {.1f, .6f, 1,   1},
        {1,   .2f, .8f, 1}
    };
    for (size_t level = 0; level < packed.levels.size(); ++level) {
        const auto& l = packed.levels[level];
        for (uint32_t index = 0; index < l.nodes.z; ++index) {
            const size_t word = (l.nodes.x + uint64_t(index) * l.nodes.y) / 4;
            if (word + 16 > packed.nodes.size())
                throw std::runtime_error("Topology viewer node outside its packed pool");
            int32_t position[3];
            std::memcpy(position, packed.nodes.data() + word + 1, sizeof(position));
            const dm::float3 low{float(position[0]), float(position[1]), float(position[2])};
            const dm::float3 high =
                low + dm::float3(float(l.nodeExtent.x), float(l.nodeExtent.y), float(l.nodeExtent.z));
            const bool section = o.mode == VolumeViewMode::Section2D;
            if (section && (settings.section.y < low.y || settings.section.y >= high.y))
                continue;
            for (unsigned corner = 0; corner < 8; ++corner)
                for (unsigned axis = 0; axis < 3; ++axis) {
                    if (corner & (1u << axis))
                        continue;
                    if (section && (axis == 1 || (corner & 2)))
                        continue;
                    const unsigned other = corner | (1u << axis);
                    const auto vertex = [&](unsigned mask) {
                        return dm::float3(mask & 1 ? high.x : low.x, mask & 2 ? high.y : low.y,
                                          mask & 4 ? high.z : low.z);
                    };
                    Line line;
                    line.color = colors[level % 4];
                    if (section) {
                        const auto a = vertex(corner), b = vertex(other);
                        const auto& info = packed.volume;
                        const float sx = o.width / (info.boundsMax.x - info.boundsMin.x),
                                    sy = o.height / (info.boundsMax.z - info.boundsMin.z);
                        line.points = {(a.x - info.boundsMin.x) * sx, (a.z - info.boundsMin.z) * sy,
                                       (b.x - info.boundsMin.x) * sx, (b.z - info.boundsMin.z) * sy};
                        // Section edges are axis aligned. Clip and reject outside edges before converting to
                        // integer tiles.
                        if ((line.points.x < 0 && line.points.z < 0) ||
                            (line.points.x > o.width && line.points.z > o.width) ||
                            (line.points.y < 0 && line.points.w < 0) ||
                            (line.points.y > o.height && line.points.w > o.height))
                            continue;
                        line.points.x = std::clamp(line.points.x, 0.f, float(o.width));
                        line.points.z = std::clamp(line.points.z, 0.f, float(o.width));
                        line.points.y = std::clamp(line.points.y, 0.f, float(o.height));
                        line.points.w = std::clamp(line.points.w, 0.f, float(o.height));
                    } else if (!projectLine(vertex(corner), vertex(other), camera, o, line))
                        continue;
                    if (lines.size() >= 1000000)
                        throw std::invalid_argument("Topology overlay exceeds one million projected lines");
                    lines.push_back(line);
                }
        }
    }
    drawOverlayLines(runtime, lines, o, p, settings, std::move(bindings));
}
void addSceneOverlay(es::ComputeRuntime& runtime, const std::vector<VolumeOverlayLine>& worldLines,
                     const std::vector<VolumeOverlayPoint>& worldPoints, const Camera& camera,
                     const VolumeViewOptions& o, const g::RaycastParameters& p, ViewSettings settings,
                     std::vector<nvrhi::BindingSetItem> bindings) {
    if (worldLines.size() + worldPoints.size() > 1000000)
        throw std::invalid_argument("Scene overlay exceeds one million primitives");
    std::vector<Line> lines;
    auto add = [&](const g::Float4& a, const g::Float4& b, const g::Float4& color) {
        const g::Float4 values[] = {a, b, color};
        for (const auto& v : values)
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || !std::isfinite(v.w))
                throw std::invalid_argument("Nonfinite scene overlay primitive");
        Line line{};
        line.color = color;
        const bool visible = o.mode == VolumeViewMode::Section2D
                                 ? projectSectionLine(a, b, p.volume, o, line)
                                 : projectLine({a.x, a.y, a.z}, {b.x, b.y, b.z}, camera, o, line);
        if (visible)
            lines.push_back(line);
    };
    for (const auto& line : worldLines)
        add(line.start, line.end, line.color);
    for (const auto& point : worldPoints)
        add(point.position, point.position, point.color);
    drawOverlayLines(runtime, lines, o, p, settings, std::move(bindings));
}
class VolumePass : public app::IRenderPass {
    NVRHI_INHERIT_INTERFACE_TABLE()
    es::ComputeRuntime m_runtime;
    std::unique_ptr<VolumeScene> m_scene;
    VolumeViewOptions m_options;
    std::string m_title;
    nvrhi::ShaderHandle m_vs, m_ps;
    nvrhi::BindingLayoutHandle m_layout;
    nvrhi::GraphicsPipelineHandle m_pipeline;
    nvrhi::CommandListHandle m_commands;
    nvrhi::BufferHandle m_constants;
    uint32_t m_frame = 0;
    int m_drag = -1, m_modifiers = 0;
    bool m_moveLight = false;
    double m_mouseX = 0, m_mouseY = 0;

 public:
    VolumePass(app::DeviceManager* manager, const SceneFactory& factory, VolumeViewOptions o, const char* title)
        : IRenderPass(manager),
          m_runtime(GetDevice(), app::GetDirectoryWithExecutable() / "shaders/gvdb_slang" /
                                     app::GetShaderTypeName(GetDevice()->getGraphicsAPI())),
          m_scene(factory(m_runtime)),
          m_options(o),
          m_title(title) {
        if (!m_scene)
            throw std::runtime_error("Volume scene factory returned null");
        auto fs = MAKE_RC_OBJ_PTR(vfs::NativeFileSystem);
        engine::ShaderFactory shaders(GetDevice(), fs,
                                      app::GetDirectoryWithExecutable() / "shaders/gvdb_volume_view" /
                                          app::GetShaderTypeName(GetDevice()->getGraphicsAPI()));
        m_vs = shaders.CreateShader("VolumePresent.slang", "main_vs", nullptr, nvrhi::ShaderType::Vertex);
        m_ps = shaders.CreateShader("VolumePresent.slang", "main_ps", nullptr, nvrhi::ShaderType::Pixel);
        if (!m_vs || !m_ps)
            throw std::runtime_error("Cannot load volume display shaders");
        nvrhi::BindingLayoutDesc layout;
        layout.visibility = nvrhi::ShaderType::Pixel;
        layout.bindings = {nvrhi::BindingLayoutItem::ConstantBuffer(0),
                           nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0)};
        check(GetDevice()->createBindingLayout(layout, &m_layout), "create volume display layout");
        nvrhi::BufferDesc desc;
        desc.byteSize = 256;
        desc.isConstantBuffer = true;
        desc.keepInitialState = true;
        desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
        check(GetDevice()->createBuffer(desc, &m_constants), "create display constants");
        check(GetDevice()->createCommandList(nvrhi::CommandListParameters(), &m_commands),
              "create display commands");
    }
    bool uiVisible() const { return m_options.showUI; }
    void flushScene() { m_runtime.flush(); }
    void buildUI() {
        m_scene->buildUI(m_options);
        // Inspector edits can rebuild a scene using the shared immediate list.
        // Close it before ImGui records its own graphics list.
        flushScene();
    }
    void cancelDrag() { m_drag = -1; }
    void capturedPointer(double x, double y) {
        cancelDrag();
        m_mouseX = x;
        m_mouseY = y;
    }
    bool KeyboardUpdate(int key, int, int action, int) override {
        if (action != GLFW_PRESS && action != GLFW_REPEAT)
            return false;
        if (m_scene->keyEvent(key, action == GLFW_REPEAT, m_options))
            return true;
        // Unhandled repeats only drive the viewer's continuous camera controls.
        if (action == GLFW_REPEAT && key != GLFW_KEY_LEFT && key != GLFW_KEY_RIGHT &&
            key != GLFW_KEY_UP && key != GLFW_KEY_DOWN)
            return false;
        if (key == GLFW_KEY_1)
            m_options.topology = !m_options.topology;
        else if (key == GLFW_KEY_V)
            m_options.mode = VolumeViewMode((uint32_t(m_options.mode) + 1) % 4);
        else if (key == GLFW_KEY_I)
            m_options.sectionInset = !m_options.sectionInset;
        else if (key == GLFW_KEY_LEFT)
            m_options.yaw -= .08f;
        else if (key == GLFW_KEY_RIGHT)
            m_options.yaw += .08f;
        else if (key == GLFW_KEY_UP)
            m_options.pitch = std::min(1.5f, m_options.pitch + .08f);
        else if (key == GLFW_KEY_DOWN)
            m_options.pitch = std::max(-1.5f, m_options.pitch - .08f);
        else
            return false;
        return true;
    }
    bool MouseButtonUpdate(int button, int action, int mods) override {
        if (button != GLFW_MOUSE_BUTTON_LEFT && button != GLFW_MOUSE_BUTTON_MIDDLE &&
            button != GLFW_MOUSE_BUTTON_RIGHT)
            return false;
        m_drag = action == GLFW_PRESS ? button : -1;
        m_modifiers = mods;
        m_moveLight = (mods & GLFW_MOD_SHIFT) != 0;
        return true;
    }
    bool MousePosUpdate(double x, double y) override {
        const float dx = float(x - m_mouseX), dy = float(y - m_mouseY);
        if (m_drag >= 0 && m_scene->mouseDrag(m_drag, dx, dy, m_modifiers, m_options)) {
            m_mouseX = x;
            m_mouseY = y;
            return true;
        }
        if (m_drag == GLFW_MOUSE_BUTTON_LEFT) {
            if (m_moveLight) {
                const auto& l = m_options.lightDirection;
                const float length = std::sqrt(l.x * l.x + l.y * l.y + l.z * l.z);
                const float yaw = std::atan2(l.x, l.z) + dx * .006f,
                            pitch = std::clamp(std::asin(l.y / length) + dy * .006f, -1.5f, 1.5f);
                m_options.lightDirection = {length * std::sin(yaw) * std::cos(pitch), length * std::sin(pitch),
                                            length * std::cos(yaw) * std::cos(pitch), 0};
            } else {
                m_options.yaw += dx * .006f;
                m_options.pitch = std::clamp(m_options.pitch + dy * .006f, -1.5f, 1.5f);
            }
        } else if (m_drag == GLFW_MOUSE_BUTTON_MIDDLE) {
            const auto snapshot = m_scene->ownedSnapshot();
            const auto c = cameraFor(snapshot ? snapshot->getDesc().volume : m_scene->volumeInfo(), m_options);
            const auto movement =
                (-c.right * dx + c.up * dy) *
                (2 * c.horizontal * c.radius * m_options.distanceScale / std::max(1u, m_options.width));
            m_options.targetOffset.x += movement.x;
            m_options.targetOffset.y += movement.y;
            m_options.targetOffset.z += movement.z;
        } else if (m_drag == GLFW_MOUSE_BUTTON_RIGHT)
            m_options.distanceScale = std::clamp(m_options.distanceScale * std::exp(dy * .01f), .1f, 50.f);
        m_mouseX = x;
        m_mouseY = y;
        m_options.sectionHeight = std::clamp(1 - float(y) / std::max(1u, m_options.height), 0.f, 1.f);
        return m_drag >= 0;
    }
    bool MouseScrollUpdate(double, double y) override {
        if (m_scene->mouseScroll(float(y), m_options))
            return true;
        m_options.distanceScale = std::clamp(m_options.distanceScale * float(std::exp(-y * .1)), .1f, 50.f);
        return true;
    }
    void BackBufferResizing() override { m_pipeline = nullptr; }
    void Animate(float elapsed) override {
        m_scene->animate(elapsed, m_options);
        const std::string title =
            m_title + " | " + m_scene->describe() +
            " | 1 topology, V shading, I section; left orbit, middle pan, right/wheel zoom, Shift light";
        GetDeviceManager()->SetInformativeWindowTitle(title.c_str());
    }
    void Render(nvrhi::IFramebuffer* framebuffer) override {
        const auto viewport = framebuffer->getFramebufferInfo().getViewport();
        m_options.width = uint32_t(viewport.width());
        m_options.height = uint32_t(viewport.height());
        auto output = renderVolumeView(m_runtime, *m_scene, m_options);
        m_runtime.flush();
        if (!m_pipeline) {
            nvrhi::GraphicsPipelineDesc desc;
            desc.VS = m_vs;
            desc.PS = m_ps;
            desc.bindingLayouts = {m_layout};
            desc.primType = nvrhi::PrimitiveType::TriangleList;
            desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
            desc.renderState.depthStencilState.depthTestEnable = false;
            desc.renderState.depthStencilState.depthWriteEnable = false;
            check(GetDevice()->createGraphicsPipeline1(desc, framebuffer->getFramebufferInfo().getInfo(),
                                                       &m_pipeline),
                  "create volume display pipeline");
        }
        nvrhi::BindingSetDesc bindings;
        bindings.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_constants),
                             nvrhi::BindingSetItem::StructuredBuffer_SRV(0, output.color)};
        nvrhi::BindingSetHandle set;
        check(GetDevice()->createBindingSet(bindings, m_layout, &set), "bind volume display");
        uint32_t data[64]{};
        data[0] = output.width;
        data[1] = output.height;
        m_commands->open();
        m_commands->writeBuffer(m_constants, data, sizeof(data));
        nvrhi::GraphicsState state;
        state.pipeline = m_pipeline;
        state.framebuffer = framebuffer;
        state.bindings = {set};
        state.viewport.addViewportAndScissorRect(viewport);
        m_commands->setGraphicsState(state);
        nvrhi::DrawArguments args;
        args.vertexCount = 3;
        m_commands->draw(args);
        m_commands->close();
        GetDevice()->executeCommandList(m_commands);
        if (m_options.frameLimit && ++m_frame >= m_options.frameLimit) {
            if (!m_options.screenshot.empty())
                writeVolumeImage(m_runtime, output, m_options.screenshot);
            if (auto* window = GetDeviceManager()->GetWindow())
                glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
    }
};
class VolumeGuiPass : public app::ImGuiRenderPass {
    NVRHI_INHERIT_INTERFACE_TABLE()
    VolumePass& m_view;

 public:
    VolumeGuiPass(app::DeviceManager* manager, VolumePass& view)
        : ImGuiRenderPass(manager), m_view(view) {}
    bool MousePosUpdate(double x, double y) override {
        const bool captured = ImGuiRenderPass::MousePosUpdate(x, y) && m_view.uiVisible();
        if (captured)
            m_view.capturedPointer(x, y);
        return captured;
    }
    bool MouseButtonUpdate(int button, int action, int mods) override {
        const bool captured = ImGuiRenderPass::MouseButtonUpdate(button, action, mods) && m_view.uiVisible();
        // A release over the Inspector must not leave a scene drag active.
        if (captured || action == GLFW_RELEASE)
            m_view.cancelDrag();
        return captured;
    }
    bool MouseScrollUpdate(double x, double y) override {
        return ImGuiRenderPass::MouseScrollUpdate(x, y) && m_view.uiVisible();
    }
    bool KeyboardUpdate(int key, int scancode, int action, int mods) override {
        return ImGuiRenderPass::KeyboardUpdate(key, scancode, action, mods) && m_view.uiVisible();
    }
    bool KeyboardCharInput(unsigned int character, int mods) override {
        return ImGuiRenderPass::KeyboardCharInput(character, mods) && m_view.uiVisible();
    }

 protected:
    void BuildUI() override {
        // Keep ImGui's frame and input state current while hiding both its window
        // and input capture. Visibility may change after the pass is registered.
        if (!m_view.uiVisible())
            return;
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Inspector"))
            m_view.buildUI();
        ImGui::End();
    }
};
// Registered passes are borrowed by this guard but retained by DeviceManager.
// Declare it after pass owners so cleanup runs before their destruction.
struct RegisteredPasses {
    app::DeviceManager* manager;
    app::IRenderPass* view;
    app::IRenderPass* gui;
    ~RegisteredPasses() {
        if (gui)
            manager->RemoveRenderPass(gui);
        manager->RemoveRenderPass(view);
        manager->GetDevice()->waitForIdle();
    }
};
struct ShutdownDevice {
    app::DeviceManager* manager;
    ~ShutdownDevice() { manager->Shutdown(); }
};
// Native-window regression used by gvdb_volume_view_tests. It exercises the real
// ImGui pass and event forwarding, without treating scene screenshots as UI proof.
void exerciseGuiInput(VolumePass& view, VolumeGuiPass& gui, app::DeviceManager& manager) {
    const auto require = [](bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    };
    // Pre-loop checks must not touch an unacquired swapchain image. Use the
    // window's format and dimensions with independently owned render storage.
    const auto& windowInfo = manager.GetFramebuffer(0)->getFramebufferInfo();
    const auto viewport = windowInfo.getViewport();
    nvrhi::TextureDesc targetDesc;
    targetDesc.width = uint32_t(viewport.width());
    targetDesc.height = uint32_t(viewport.height());
    targetDesc.format = windowInfo.colorFormats[0];
    targetDesc.isRenderTarget = true;
    targetDesc.initialState = nvrhi::ResourceStates::RenderTarget;
    targetDesc.keepInitialState = true;
    nvrhi::TextureHandle target;
    check(manager.GetDevice()->createTexture(targetDesc, &target), "create GUI test target");
    nvrhi::FramebufferHandle framebuffer;
    check(manager.GetDevice()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target), &framebuffer),
          "create GUI test framebuffer");
    const auto draw = [&]() {
        gui.Animate(0);
        gui.Render(framebuffer);
        return ImGui::GetDrawData()->TotalVtxCount;
    };
    view.flushScene();
    nvrhi::CommandListHandle clear;
    check(manager.GetDevice()->createCommandList(nvrhi::CommandListParameters(), &clear), "create GUI test clear");
    clear->open();
    clear->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(0.f));
    clear->close();
    manager.GetDevice()->executeCommandList(clear);
    (void)draw(); // ImGui sizes a newly created window on its first frame.
    require(draw() > 0, "Visible Inspector has no draw geometry");
    auto& io = ImGui::GetIO();
    io.WantCaptureKeyboard = io.WantCaptureMouse = true;
    require(gui.KeyboardUpdate(GLFW_KEY_G, 0, GLFW_PRESS, 0) &&
                gui.KeyboardCharInput('g', 0) && gui.MousePosUpdate(40, 40) &&
                gui.MouseButtonUpdate(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0) &&
                gui.MouseScrollUpdate(0, 1),
            "Visible Inspector did not capture requested input");
    gui.KeyboardUpdate(GLFW_KEY_G, 0, GLFW_RELEASE, 0);
    gui.MouseButtonUpdate(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
    view.KeyboardUpdate(GLFW_KEY_GRAVE_ACCENT, 0, GLFW_PRESS, 0);
    require(!view.uiVisible(), "Fixture did not hide the Inspector");
    view.KeyboardUpdate(GLFW_KEY_GRAVE_ACCENT, 0, GLFW_REPEAT, 0);
    require(!view.uiVisible(), "Key repeat toggled the Inspector a second time");
    require(view.KeyboardUpdate(GLFW_KEY_LEFT, 0, GLFW_REPEAT, 0), "Camera navigation lost key repeat");
    // Test the transition before a new ImGui frame updates its stale capture flags.
    io.WantCaptureKeyboard = io.WantCaptureMouse = true;
    require(!gui.KeyboardUpdate(GLFW_KEY_G, 0, GLFW_PRESS, 0), "Hidden Inspector captured a key");
    view.KeyboardUpdate(GLFW_KEY_G, 0, GLFW_PRESS, 0);
    require(!view.KeyboardUpdate(GLFW_KEY_G, 0, GLFW_REPEAT, 0), "Repeated scene action was forwarded");
    require(!gui.KeyboardCharInput('g', 0), "Hidden Inspector captured character input");
    require(!gui.MouseButtonUpdate(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0),
            "Hidden Inspector captured a mouse press");
    view.MouseButtonUpdate(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
    require(!gui.MousePosUpdate(48, 43), "Hidden Inspector captured mouse movement");
    view.MousePosUpdate(48, 43);
    require(!gui.MouseScrollUpdate(0, 1), "Hidden Inspector captured scrolling");
    view.MouseScrollUpdate(0, 1);
    gui.KeyboardUpdate(GLFW_KEY_G, 0, GLFW_RELEASE, 0);
    gui.MouseButtonUpdate(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
    view.MouseButtonUpdate(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
    require(draw() == 0, "Hidden Inspector still emits draw geometry");
    view.KeyboardUpdate(GLFW_KEY_GRAVE_ACCENT, 0, GLFW_PRESS, 0);
    require(view.uiVisible() && draw() > 0, "Inspector did not return after showing it");
    manager.GetDevice()->waitForIdle();
}
}  // namespace
g::RaycastOutput renderVolumeView(es::ComputeRuntime& runtime, VolumeScene& scene, const VolumeViewOptions& o) {
    if (!o.width || !o.height || uint64_t(o.width) * o.height > 16 * 1024 * 1024 || !std::isfinite(o.step) ||
        o.step <= 0 || !std::isfinite(o.yaw) || !std::isfinite(o.pitch) || std::abs(o.pitch) > 1.5f ||
        !std::isfinite(o.distanceScale) || o.distanceScale <= 0 || !std::isfinite(o.sectionHeight) ||
        o.sectionHeight < 0 || o.sectionHeight > 1 || uint32_t(o.mode) > uint32_t(VolumeViewMode::Section2D))
        throw std::invalid_argument("Invalid volume viewer dimensions, camera, mode or step");
    if (!std::isfinite(o.scalarBackground))
        throw std::invalid_argument("Nonfinite sparse background");
    if (o.explicitCamera) {
        const g::Float4 vectors[] = {o.cameraEye, o.cameraForward, o.cameraUp};
        for (const auto& v : vectors)
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
                throw std::invalid_argument("Nonfinite explicit camera");
        const dm::float3 forward(o.cameraForward.x, o.cameraForward.y, o.cameraForward.z),
            up(o.cameraUp.x, o.cameraUp.y, o.cameraUp.z);
        const float ff = dm::dot(forward, forward), uu = dm::dot(up, up);
        const auto cross = dm::cross(forward, up);
        if (!std::isfinite(ff) || !std::isfinite(uu) || ff < 1e-12f || uu < 1e-12f ||
            dm::dot(cross, cross) <= ff * uu * 1e-8f || !std::isfinite(o.horizontalFov) ||
            o.horizontalFov <= 0 || o.horizontalFov >= 179 || !std::isfinite(o.cameraNear) ||
            !std::isfinite(o.cameraFar) || o.cameraNear <= 0 || o.cameraFar <= o.cameraNear)
            throw std::invalid_argument("Invalid explicit camera basis, field of view or clip interval");
    }
    const g::Float4 extra[] = {o.targetOffset, o.lightDirection};
    for (const auto& x : extra)
        if (!std::isfinite(x.x) || !std::isfinite(x.y) || !std::isfinite(x.z))
            throw std::invalid_argument("Invalid viewer translation or light");
    if (o.lightDirection.x == 0 && o.lightDirection.y == 0 && o.lightDirection.z == 0)
        throw std::invalid_argument("Zero viewer light direction");
    auto transfer = scene.transfer();
    const auto snapshot = scene.ownedSnapshot();
    const auto channel = scene.ownedScalarChannel();
    const auto info = snapshot ? snapshot->getDesc().volume : scene.volumeInfo();
    const auto camera = cameraFor(info, o);
    auto p = parameters(info, o, camera, transfer.size());
    g::validateRaycastParameters(p, transfer);
    const auto overlayLines = scene.overlayLines();
    const auto overlayPoints = scene.overlayPoints();
    g::RaycastOutput output;
    bool customSelected = false;
    if (o.mode == VolumeViewMode::Volume || o.mode == VolumeViewMode::Surface) {
        output = scene.renderCustom(p, o);
        if (output.color || output.depth) {
            customSelected = true;
            const uint64_t pixels = uint64_t(o.width) * o.height;
            if (!output.color || !output.depth || output.width != o.width || output.height != o.height ||
                output.color->getDesc().structStride != sizeof(g::Float4) ||
                output.depth->getDesc().structStride != sizeof(float) ||
                output.color->getDesc().byteSize < pixels * sizeof(g::Float4) ||
                output.depth->getDesc().byteSize < pixels * sizeof(float))
                throw std::invalid_argument("Custom scene output does not match the viewer image");
        } else if (snapshot) {
            if (o.mode == VolumeViewMode::Surface && (o.scalarBackground != 0 || scene.colorAtlas()))
                throw std::invalid_argument("Owned colored/background level sets require a custom surface renderer");
            output = raycastOwnedVolume(runtime, snapshot, channel, p, transfer, o.mode == VolumeViewMode::Surface);
        } else if (o.mode == VolumeViewMode::Surface && (o.scalarBackground != 0 || scene.colorAtlas()))
            output = renderVolumeSurface(runtime, scene.volume(), p, transfer, scene.colorAtlas(), o.scalarBackground);
        else
            output = scene.volume().raycast(p, transfer, o.mode == VolumeViewMode::Surface);
        if (!o.sectionInset && !o.topology && overlayLines.empty() && overlayPoints.empty())
            return output;
    } else if (snapshot) {
        const auto mode = o.mode == VolumeViewMode::Voxel ? OwnedVolumeViewMode::Voxel
            : o.mode == VolumeViewMode::Section ? OwnedVolumeViewMode::Section : OwnedVolumeViewMode::Section2D;
        output = renderOwnedVolumeView(runtime, snapshot, channel, p, transfer, mode,
            info.boundsMin.y + (info.boundsMax.y - info.boundsMin.y) * o.sectionHeight);
    } else {
        output.width = o.width;
        output.height = o.height;
        output.color = runtime.buffer(size_t(o.width) * o.height * sizeof(g::Float4), sizeof(g::Float4));
        output.depth = runtime.buffer(size_t(o.width) * o.height * sizeof(float), sizeof(float));
    }
    std::vector<nvrhi::BindingSetItem> bindings = {
        nvrhi::BindingSetItem::StructuredBuffer_UAV(0, output.color),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(1, output.depth)};
    // Screen overlays only need the output image. A custom snapshot renderer
    // therefore needs no duplicate typed atlas or legacy SlangVolume instance.
    nvrhi::BufferHandle transferBuffer;
    if (!snapshot && (!customSelected || o.sectionInset)) {
        auto& volume = scene.volume();
        transferBuffer =
            runtime.buffer(transfer.size() * sizeof(g::Float4), sizeof(g::Float4), transfer.data());
        bindings.insert(bindings.end(), {
            nvrhi::BindingSetItem::StructuredBuffer_SRV(0, volume.atlas()),
            nvrhi::BindingSetItem::RawBuffer_SRV(1, volume.nodes()),
            nvrhi::BindingSetItem::RawBuffer_SRV(2, volume.children()),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(3, volume.levels()),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(4, transferBuffer)});
    }
    ViewSettings settings{};
    settings.rectangle = {0, 0, o.width, o.height};
    settings.section = {0, info.boundsMin.y + (info.boundsMax.y - info.boundsMin.y) * o.sectionHeight, 0, 0};
    auto constants = settingsBuffer(runtime, settings);
    auto extended = bindings;
    extended.push_back(nvrhi::BindingSetItem::ConstantBuffer(1, constants));
    if (!snapshot && (o.mode == VolumeViewMode::Voxel || o.mode == VolumeViewMode::Section ||
        o.mode == VolumeViewMode::Section2D)) {
        const char* entry = o.mode == VolumeViewMode::Voxel     ? "renderVoxel"
                            : o.mode == VolumeViewMode::Section ? "renderSection"
                                                                : "renderSection2D";
        runtime.dispatch("VolumeView", entry, &p, sizeof(p), extended, (o.width + 7) / 8, (o.height + 7) / 8);
    }
    if (o.topology) {
        std::vector<VolumeOverlayLine> topology;
        if (scene.topologyOverlay(topology))
            addSceneOverlay(runtime, topology, {}, camera, o, p, settings, bindings);
        else
            addTopology(runtime, scene.packed(), camera, o, p, settings, bindings);
    }
    addSceneOverlay(runtime, overlayLines, overlayPoints, camera, o, p, settings, bindings);
    if (o.sectionInset && o.mode != VolumeViewMode::Section2D) {
        const uint32_t width = std::max(1u, o.width / 4), height = std::max(1u, o.height / 4);
        settings.rectangle = {0, o.height - height, width, height};
        if (snapshot) {
            drawOwnedVolumeSection(runtime, snapshot, channel, p, transfer, settings.section.y, settings.rectangle, output);
        } else {
            constants = settingsBuffer(runtime, settings);
            extended = bindings;
            extended.push_back(nvrhi::BindingSetItem::ConstantBuffer(1, constants));
            runtime.dispatch("VolumeView", "renderSection2D", &p, sizeof(p), extended, (width + 7) / 8,
                             (height + 7) / 8);
        }
    }
    return output;
}
void writeVolumeImage(es::ComputeRuntime& runtime, const g::RaycastOutput& output,
                      const std::filesystem::path& path) {
    std::vector<g::Float4> color(size_t(output.width) * output.height);
    runtime.read(output.color, color.data(), color.size() * sizeof(color[0]));
    std::vector<uint8_t> bytes(color.size() * 4);
    for (size_t i = 0; i < color.size(); ++i) {
        const float values[4] = {color[i].x, color[i].y, color[i].z, color[i].w};
        for (unsigned c = 0; c < 4; ++c) {
            if (!std::isfinite(values[c]))
                throw std::runtime_error("Nonfinite volume display pixel");
            bytes[i * 4 + c] = uint8_t(std::clamp(values[c], 0.f, 1.f) * 255 + .5f);
        }
    }
    if (!stbi_write_png(path.string().c_str(), int(output.width), int(output.height), 4, bytes.data(),
                        int(output.width) * 4))
        throw std::runtime_error("Cannot write volume image: " + path.string());
}

std::vector<uint8_t> renderVolumeOffscreen(int argc, const char** argv, const SceneFactory& factory,
                                           VolumeViewOptions options) {
    if (!options.width || !options.height || uint64_t(options.width) * options.height > 16 * 1024 * 1024)
        throw std::invalid_argument("Invalid volume display dimensions");
    int adapter = -1;
    bool validation = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-cuda"))
            throw std::invalid_argument("CUDA has no graphics presenter");
        if (!std::strcmp(argv[i], "--validation"))
            validation = true;
        if (!std::strcmp(argv[i], "--adapter")) {
            if (++i == argc)
                throw std::invalid_argument("Missing adapter");
            adapter = std::stoi(argv[i]);
            if (adapter < 0)
                throw std::invalid_argument("Negative adapter");
        }
    }
    auto manager = nvrhi::TakeOver(app::DeviceManager::Create(app::GetGraphicsAPIFromCommandLine(argc, argv)));
    if (!manager)
        throw std::runtime_error("Requested graphics API is unavailable");
    ShutdownDevice shutdown{manager.Get()};
    app::DeviceCreationParameters params;
    params.adapterIndex = adapter;
    params.enableDebugRuntime = validation;
    params.enableNvrhiValidationLayer = true;
    params.enableRayTracingExtensions = options.requestRayTracing;
    if (!manager->CreateHeadlessDevice(params))
        throw std::runtime_error("Cannot create volume display device");
    std::vector<uint8_t> result(size_t(options.width) * options.height * 4);
    {
        auto* device = manager->GetDevice();
        VolumePass view(manager, factory, options, "Volume readback");
        nvrhi::TextureDesc desc;
        desc.width = options.width;
        desc.height = options.height;
        desc.format = nvrhi::Format::RGBA8_UNORM;
        desc.isRenderTarget = true;
        desc.initialState = nvrhi::ResourceStates::RenderTarget;
        desc.keepInitialState = true;
        nvrhi::TextureHandle color;
        check(device->createTexture(desc, &color), "create volume display target");
        nvrhi::FramebufferHandle framebuffer;
        check(device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(color), &framebuffer),
              "create display framebuffer");
        view.Animate(0);
        view.Render(framebuffer);
        nvrhi::StagingTextureHandle staging;
        check(device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read, &staging),
              "create display readback");
        nvrhi::CommandListHandle commands;
        check(device->createCommandList(nvrhi::CommandListParameters(), &commands), "create readback commands");
        commands->open();
        commands->copyTexture2(staging, {}, color, {});
        commands->close();
        device->executeCommandList(commands);
        if (!device->waitForIdle())
            throw std::runtime_error("Volume display execution failed");
        size_t pitch = 0;
        const auto* data = static_cast<const uint8_t*>(
            device->mapStagingTexture(staging, {}, nvrhi::CpuAccessMode::Read, pitch));
        if (!data)
            throw std::runtime_error("Cannot map volume display image");
        if (pitch < options.width * 4) {
            device->unmapStagingTexture(staging);
            throw std::runtime_error("Short volume image row pitch");
        }
        for (uint32_t y = 0; y < options.height; ++y)
            std::memcpy(result.data() + size_t(y) * options.width * 4, data + size_t(y) * pitch,
                        size_t(options.width) * 4);
        device->unmapStagingTexture(staging);
    }
    return result;
}

int runVolumeWindow(int argc, const char** argv, const char* title, const SceneFactory& factory,
                    VolumeViewOptions options) {
    if (!options.width || !options.height || uint64_t(options.width) * options.height > 16 * 1024 * 1024)
        throw std::invalid_argument("Invalid volume display dimensions");
    log::ConsoleApplicationMode();
    int adapter = -1;
    bool validation = false, guiInputTest = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-cuda"))
            throw std::invalid_argument("CUDA volume rendering requires --headless");
        if (!std::strcmp(argv[i], "--validation"))
            validation = true;
        if (!std::strcmp(argv[i], "--gui-input-test"))
            guiInputTest = true;
        if (!std::strcmp(argv[i], "--adapter")) {
            if (++i == argc)
                throw std::invalid_argument("Missing adapter");
            adapter = std::stoi(argv[i]);
            if (adapter < 0)
                throw std::invalid_argument("Negative adapter");
        }
    }
    auto manager = nvrhi::TakeOver(app::DeviceManager::Create(app::GetGraphicsAPIFromCommandLine(argc, argv)));
    if (!manager)
        throw std::runtime_error("Requested graphics API is unavailable");
    ShutdownDevice shutdown{manager.Get()};
    app::DeviceCreationParameters p;
    p.adapterIndex = adapter;
    p.enableDebugRuntime = validation;
    p.enableNvrhiValidationLayer = true;
    p.enableRayTracingExtensions = options.requestRayTracing;
    p.backBufferWidth = options.width;
    p.backBufferHeight = options.height;
    if (!manager->CreateWindowDeviceAndSwapChain(p, title))
        throw std::runtime_error("Cannot create volume window");
    {
        VolumePass view(manager, factory, options, title);
        auto fs = MAKE_RC_OBJ_PTR(vfs::RootFileSystem);
        fs->mount("donut", app::GetDirectoryWithExecutable() / "shaders/framework" /
                               app::GetShaderTypeName(manager->GetDevice()->getGraphicsAPI()));
        engine::ShaderFactory guiShaders(manager->GetDevice(), fs, "");
        nvrhi::AutoPtr<VolumeGuiPass> gui;
        if (options.showUI) {
            gui = MAKE_RC_OBJ_PTR(VolumeGuiPass, manager.Get(), view);
            if (!gui->Init(&guiShaders))
                throw std::runtime_error("Cannot initialize the volume Inspector");
            // Keep bounded tests and captures independent of a previous window layout.
            ImGui::GetIO().IniFilename = nullptr;
        }
        RegisteredPasses registered{manager.Get(), &view, gui.Get()};
        manager->AddRenderPassToBack(&view);
        if (gui)
            manager->AddRenderPassToBack(gui.Get());
        if (guiInputTest) {
            if (!gui)
                throw std::invalid_argument("GUI input test requires an enabled Inspector");
            exerciseGuiInput(view, *gui, *manager);
        }
        manager->RunMessageLoop();
    }
    return 0;
}
}  // namespace gvdb::sample

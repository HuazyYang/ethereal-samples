#ifndef SAMPLETYPES_H
#define SAMPLETYPES_H
#include <nvrhi/core/foundation.h>
#include <gvdb/GPDevice.h>
#include <donut/core/log.h>
#include <nvrhi/core/memory.h>
#include <donut/core/math/math.h>
#include <gvdb/GVDBScene.h>
#include <string>

namespace SampleUtils {

// Index -> application space of a VBXTransform (the reference SetTransform:
// pretranslate, then scale, then rotate ZYX, then translate), row-vector convention.
dm::affine3 vbxTransformToAffine(const gvdb::VBXTransform &xf);
// The reference's explicit form: SetTransform(pretrans, scale, anglesDeg, trans).
dm::affine3 makeVolumeTransform(dm::float3 pretrans, dm::float3 scale, dm::float3 anglesDeg, dm::float3 trans);

// Attaches a volume instance under a new node of the graph's root (node
// transform = indexToWorld) and refreshes the graph.
nvrhi::AutoPtr<gvdb::GVDBVolumeInstance> attachVolumeInstance(gvdb::GVDBSceneGraph *graph, gvdb::IGVDBVolume *volume,
                                                             const dm::affine3 &indexToWorld, const std::string &name);
// Sets the node transform of an attached leaf from an affine (decomposed into T R S).
void setNodeTransform(donut::engine::SceneGraphNode *node, const dm::affine3 &transform);

// Host-side wait for everything enqueued on the queue so far. IDevice::waitForQueue
// only waits for the sync points recorded by commitQueue, so a fresh one is
// committed first.
inline void syncQueue(donut::gp::IDevice *device, donut::gp::IDeviceQueue *queue) {
    if (!device || !queue) return;
    device->commitQueue(queue);
    device->waitForQueue(queue);
}

// Shared-resource flags for an nvrhi texture / buffer that CUDA imports:
//   D3D11  -> NT handle + keyed mutex (the gp interop synchronises with it),
//   D3D12  -> shared heap,
//   Vulkan -> exactly `Shared` (nvrhi's Vulkan backend only adds the external
//             memory info when the flags equal Shared).
inline nvrhi::SharedResourceFlags interopSharedFlags(nvrhi::IDevice *device) {
    switch (device->getGraphicsAPI()) {
        case nvrhi::GraphicsAPI::D3D11: return nvrhi::SharedResourceFlags::Shared | nvrhi::SharedResourceFlags::Shared_NTHandle;
        default: return nvrhi::SharedResourceFlags::Shared;
    }
}

// RGBA8 packing of the reference COLORA macro (r in the low byte).
inline uint32_t packColorA(float r, float g, float b, float a) {
    auto c = [](float v) { return uint32_t(dm::clamp(v, 0.f, 1.f) * 255.f); };
    return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

#define UT_V_GP(expr)                                                      \
    do {                                                                     \
        auto rc = (expr);                                                    \
        if (NVRHI_FAILED(rc)) {                                                   \
            donut::log::error("GPDevice failed with error: %d", (int)rc); \
            NVRHI_ASSERT(0);                                              \
        }                                                                    \
    } while (0)

struct GPDeviceMessageCallback : public nvrhi::ObjectImpl<donut::gp::IMessageCallback> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GPDeviceMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(donut::gp::IMessageCallback)
    NVRHI_END_INTERFACE_TABLE()

    void message(donut::gp::MessageSeverity severity, const char *desc) override {
        using namespace donut;
        log::Severity logSeverity;
        switch (severity) {
            default:
            case gp::MessageSeverity::Info:
                logSeverity = log::Severity::Info;
                break;
            case gp::MessageSeverity::Warning:
                logSeverity = log::Severity::Warning;
                break;
            case gp::MessageSeverity::Error:
                logSeverity = log::Severity::Error;
                break;
            case gp::MessageSeverity::Fatal:
                logSeverity = log::Severity::Fatal;
        }

        donut::log::message(logSeverity, "%s", desc);
    }
};

template <typename T>
class Range {
public:
    Range(T *start, T *end): _start{start}, _end{end}, _count{size_t(end - start)} {}
    Range(T *start, size_t n) : _start{start}, _end{start + n}, _count{n} {}

    T *data() { return _start; }
    const T *data() const { return _start; }

    size_t size() const { return _count; }

    T &operator[](size_t i) {
        if (i < _count) return _start[i];
        NVRHI_ASSERT(0 && "Invalid index");
        throw std::invalid_argument("Range invalid index");
    }

    const T &operator[](size_t i) const {
        if (i < _count) return _start[i];
        NVRHI_ASSERT(0 && "Invalid index");
        throw std::invalid_argument("Range invalid index");
    }

    T *begin() { return _start; }

    T *end() { return _end; }

    const T *begin() const { return _start; }
    const T *end() const { return _end; }

 private:
    T *_start, *_end;
    size_t _count;
};

};  // namespace SampleUtils

#endif /* SAMPLETYPES_H */

#ifndef SAMPLETYPES_H
#define SAMPLETYPES_H
#include <nvrhi/core/foundation.h>
#include <gvdb/GVDBCompute.h>
#include <donut/core/log.h>
#include <nvrhi/core/memory.h>
#include <donut/core/math/math.h>
#include <gvdb/GVDBScene.h>
#include <string>

// Shader containers of sample-utils (kernels/*.slang), next to the library's ones.
#define GVDB_SHADERS_RAYCAST "shaders/gvdb/GVDBRaycast.nvsc"
#define GVDB_SHADERS_OPTIX "shaders/gvdb/OptixPrograms.nvsc"

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

// Host-side wait for the GVDB work recorded so far (submits it first).
inline void syncCompute(gvdb::IComputeContext *context) {
    if (context) context->finish();
}

// Shared-resource flags for an nvrhi texture / buffer that the CUDA device imports (IInteropDevice):
// D3D12 and Vulkan only; GVDBApp refuses D3D11.
inline nvrhi::SharedResourceFlags interopSharedFlags() { return nvrhi::SharedResourceFlags::Shared; }

// RGBA8 packing of the reference COLORA macro (r in the low byte).
inline uint32_t packColorA(float r, float g, float b, float a) {
    auto c = [](float v) { return uint32_t(dm::clamp(v, 0.f, 1.f) * 255.f); };
    return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

// Logs a failed FRESULT call.
#define UT_CHECK(expr)                                                       \
    do {                                                                     \
        auto rc = (expr);                                                    \
        if (NVRHI_FAILED(rc)) {                                              \
            donut::log::error("%s failed with error %d", #expr, (int)rc);   \
            NVRHI_ASSERT(0);                                                 \
        }                                                                    \
    } while (0)

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

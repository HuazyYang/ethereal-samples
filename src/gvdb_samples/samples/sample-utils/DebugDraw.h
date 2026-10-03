#ifndef SAMPLE_UTILS_DEBUGDRAW_H
#define SAMPLE_UTILS_DEBUGDRAW_H
// Immediate-mode 3D lines (the draw3D / drawLine3D / drawBox3DXform helpers
// of the reference nv_gui), drawn with shaders/Lines.hlsl on top of the
// presented frame. Collect during the frame, flush once.
#include <nvrhi/nvrhi.h>
#include <donut/core/math/math.h>
#include <donut/engine/ShaderFactory.h>
#include <gvdb/GVDBScene.h>
#include <vector>

namespace SampleUtils {

class DebugDraw {
 public:
    DebugDraw(nvrhi::IDevice *device, donut::engine::ShaderFactory *shaderFactory,
              const nvrhi::FramebufferInfoEx &fbInfo);
    ~DebugDraw();

    void line(dm::float3 a, dm::float3 b, dm::float4 color);
    // 12 edges of the box; xform maps the box corners to world space.
    void box(dm::float3 bmin, dm::float3 bmax, dm::float4 color, const dm::affine3 *xform = nullptr);
    // All nodes of all levels of the volume, coloured per level, in the instance's world transform.
    void topology(gvdb::GVDBVolumeInstance *instance);
    void topology(gvdb::IGVDBVolume *volume, const dm::affine3 &indexToWorld);
    // Camera frustum edges from the eye through the four corner rays.
    void frustum(dm::float3 eye, dm::float3 rayTL, dm::float3 rayU, dm::float3 rayV, float length, dm::float4 color);

    size_t getLineCount() const { return m_vertices.size() / 2; }
    // Uploads and draws the collected lines with the view-projection matrix, then clears them.
    void flush(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, const dm::float4x4 &viewProj);
    void clear() { m_vertices.clear(); }

 private:
    struct Vertex {
        dm::float3 position;
        dm::float4 color;
    };
    std::vector<Vertex> m_vertices;
    nvrhi::DeviceHandle m_device;
    nvrhi::BufferHandle m_vertexBuffer;
    nvrhi::BufferHandle m_constantBuffer;
    nvrhi::GraphicsPipelineHandle m_pipeline;
    nvrhi::BindingLayoutHandle m_bindingLayout;
    nvrhi::BindingSetHandle m_bindingSet;
    nvrhi::InputLayoutHandle m_inputLayout;
    size_t m_vertexCapacity = 0;
};

}  // namespace SampleUtils

#endif /* SAMPLE_UTILS_DEBUGDRAW_H */

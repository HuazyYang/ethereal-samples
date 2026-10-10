#ifndef SPH_FLUID_SCENE_H
#define SPH_FLUID_SCENE_H
// SPH particle sets in a Donut scene graph, and the rasterizer that draws them.
//
// Same design as gvdb/GVDBScene.h: ISPHParticles is shared data (as MeshInfo
// or IGVDBVolume), SPHFluidInstance is the SceneGraphLeaf that places one
// particle set in the world. The leaves attach to any SceneGraph, in the
// samples to a gvdb::GVDBSceneGraph next to volumes, meshes, cameras and
// lights. The node transform maps the fluid's world units to scene space.
#include "SPHFluid.h"
#include <donut/engine/SceneGraph.h>
#include <donut/engine/ShaderFactory.h>
#include <memory>
#include <vector>

namespace sph {

// Content flag of particle leaves. Outside the bits Donut defines (0x01..0x20)
// and next to gvdb::SceneContentFlags_Volumes (0x100).
constexpr donut::engine::SceneContentFlags SceneContentFlags_Particles = donut::engine::SceneContentFlags(0x200);

// How one instance is drawn (the uniforms of the reference's point shader).
struct SPHRenderAttributes {
    // The reference adds |velocity|^2 / 20 to the particle colour.
    float velocityTint = 1.f / 20.f;
    bool visible = true;
};

NVRHI_CLASS_CLSID(SPHFluidInstance, "942d1de1-42fa-49e1-95ad-1167378ef570")
class SPHFluidInstance : public donut::engine::SceneGraphLeaf {
 public:
    NVRHI_DECLARE_UUID_TRAITS(SPHFluidInstance)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(SPHFluidInstance)
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IWeakReferenceSource)
    NVRHI_IMPLEMENTS_CLASS(SPHFluidInstance)
    NVRHI_IMPLEMENTS_ROUTE_PARENT(donut::engine::SceneGraphLeaf)
    NVRHI_END_INTERFACE_TABLE()

    explicit SPHFluidInstance(ISPHParticles *particles) : m_particles(particles) {}

    [[nodiscard]] ISPHParticles *GetParticles() const { return m_particles.Get(); }
    [[nodiscard]] SPHRenderAttributes &GetRenderAttributes() { return m_attributes; }
    [[nodiscard]] const SPHRenderAttributes &GetRenderAttributes() const { return m_attributes; }

    // The simulation domain (bound_min .. bound_max).
    [[nodiscard]] dm::box3 GetLocalBoundingBox() override;
    [[nodiscard]] nvrhi::AutoPtr<SceneGraphLeaf> Clone() override;
    [[nodiscard]] donut::engine::SceneContentFlags GetContentFlags() const override {
        return SceneContentFlags_Particles;
    }

    // Node transform (identity when the instance is not attached).
    [[nodiscard]] dm::affine3 GetLocalToWorld() const;

 private:
    nvrhi::AutoPtr<ISPHParticles> m_particles;
    SPHRenderAttributes m_attributes;
};

// Collects the particle leaves below `root` (depth first, in graph order).
void collectFluidInstances(donut::engine::SceneGraphNode *root, std::vector<SPHFluidInstance *> &outInstances);

// Draws particle sets as depth-tested points coloured by their packed colour
// plus a velocity tint (Particles::Draw and the SPNT shader of the reference).
// Reads the shared Position / Color / Velocity vertex buffers directly, so the
// particle sets must be configured with an interop device.
//
// Shaders: "sph_fluid_pool/Points.hlsl" (main_vs, main_ps) from the factory.
class SPHPointRenderer {
 public:
    SPHPointRenderer(nvrhi::IDevice *device, donut::engine::ShaderFactory *shaderFactory);
    ~SPHPointRenderer();

    // Draws every visible instance into `framebuffer` (which must have a depth
    // attachment). `viewProj` maps scene space to clip space (row vectors).
    // The command list is open; the caller owns begin / end of the frame and
    // the synchronisation with the compute queue.
    void render(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer,
                const std::vector<SPHFluidInstance *> &instances, const dm::float4x4 &viewProj);

 private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace sph

#endif /* SPH_FLUID_SCENE_H */

#ifndef SAMPLE_UTILS_SCREENPRESENTER_H
#define SAMPLE_UTILS_SCREENPRESENTER_H
// Brings CUDA / OptiX render buffers (RGBA8 buffers of the CUDA device) to the
// nvrhi swap chain: each target owns a shared texture of the graphics device,
// imported into the CUDA device through nvrhi::IInteropDevice; per frame the
// compute context copies the buffer into the texture, and the graphics queue
// blits it with a full-screen quad (shaders/FullScreen.hlsl). The two queues
// are ordered by the timeline fences of the interop device.
#include <nvrhi/nvrhi.h>
#include <nvrhi/interop.h>
#include <nvrhi/core/autoptr.h>
#include <donut/core/math/math.h>
#include <donut/engine/ShaderFactory.h>
#include <gvdb/GVDBCompute.h>
#include <memory>
#include <vector>

namespace SampleUtils {

class ScreenPresenter {
 public:
    ScreenPresenter(nvrhi::IDevice *device, nvrhi::IInteropDevice *interop, gvdb::IComputeContext *compute,
                    donut::engine::ShaderFactory *shaderFactory, const nvrhi::FramebufferInfoEx &fbInfo);
    ~ScreenPresenter();

    // Targets: id 0 is created by the app for the full window; samples add
    // insets (e.g. the cross-section view of 3d-print).
    int createTarget(uint32_t width, uint32_t height);
    void resizeTarget(int id, uint32_t width, uint32_t height);
    uint32_t getWidth(int id) const;
    uint32_t getHeight(int id) const;
    // RGBA8 buffer (width*height*4) of the CUDA device that the renderers write into.
    nvrhi::IBuffer *getRenderBuffer(int id) const;
    // The shared texture on the graphics device.
    nvrhi::ITexture *getTexture(int id) const;

    // Frame protocol (see GVDBApp::Render):
    //   beginComputeFrame();   CUDA waits until the graphics queue has read the textures of the previous frame
    //   ... renderers write the render buffers through the compute context ...
    //   endComputeFrame();     buffers -> textures on CUDA, submitted; the graphics queue waits for them
    //   blit(...) on the graphics command list
    void beginComputeFrame();
    void endComputeFrame();
    // Draws target `id` into the normalised rectangle of the framebuffer
    // (0,0 = top-left, 1,1 = bottom-right).
    void blit(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, int id,
              dm::box2 dstRect = dm::box2(dm::float2(0.f), dm::float2(1.f)));

 private:
    struct Target;
    nvrhi::DeviceHandle m_device;
    nvrhi::InteropDeviceHandle m_interop;
    nvrhi::AutoPtr<gvdb::IComputeContext> m_compute;
    gvdb::IComputeKernel *m_copyKernel = nullptr;
    std::vector<std::unique_ptr<Target>> m_targets;
    nvrhi::GraphicsPipelineHandle m_pipeline;
    nvrhi::BindingLayoutHandle m_bindingLayout;
    nvrhi::SamplerHandle m_sampler;
};

}  // namespace SampleUtils

#endif /* SAMPLE_UTILS_SCREENPRESENTER_H */

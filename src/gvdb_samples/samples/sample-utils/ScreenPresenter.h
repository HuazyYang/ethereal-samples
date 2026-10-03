#ifndef SAMPLE_UTILS_SCREENPRESENTER_H
#define SAMPLE_UTILS_SCREENPRESENTER_H
// Brings CUDA / OptiX render buffers (RGBA8 in gp buffers) to the nvrhi swap
// chain: each target owns a shared nvrhi texture with its gp view; per frame
// the gp queue copies the buffer into the texture under the keyed mutex /
// semaphore protocol of IGPAndNVRHIInteropDevice, and the graphics queue
// blits it with a full-screen quad (shaders/FullScreen.hlsl).
#include <nvrhi/nvrhi.h>
#include <nvrhi/core/autoptr.h>
#include <donut/core/math/math.h>
#include <donut/engine/ShaderFactory.h>
#include <gvdb/GPDevice.h>
#include <gvdb/GPDeviceNVRHI.h>
#include <vector>

namespace SampleUtils {

class ScreenPresenter {
 public:
    ScreenPresenter(nvrhi::IDevice *device, donut::IGPAndNVRHIInteropDevice *interop, donut::gp::IDevice *gpDevice,
                    donut::gp::IDeviceQueue *gpQueue, donut::engine::ShaderFactory *shaderFactory,
                    const nvrhi::FramebufferInfoEx &fbInfo);
    ~ScreenPresenter();

    // Targets: id 0 is created by the app for the full window; samples add
    // insets (e.g. the cross-section view of 3d-print).
    int createTarget(uint32_t width, uint32_t height);
    void resizeTarget(int id, uint32_t width, uint32_t height);
    uint32_t getWidth(int id) const;
    uint32_t getHeight(int id) const;
    // RGBA8 buffer (width*height*4) the renderers write into.
    donut::gp::IBuffer *getRenderBuffer(int id) const;
    nvrhi::ITexture *getTexture(int id) const;

    // Frame protocol (see GVDBApp::Render):
    //   beginGPFrame();            gp queue waits for the previous graphics frame
    //   ... renderers write the render buffers on the gp queue ...
    //   endGPFrame();              buffers -> textures, gp queue signals
    //   open command list; beginGraphics(cmd);   graphics waits, acquires the textures
    //   blit(...) / overlays;      endGraphics(cmd); close + execute; endGraphicsFrame();
    void beginGPFrame();
    void endGPFrame();
    void beginGraphics(nvrhi::ICommandList *commandList);
    // Draws target `id` into the normalised rectangle of the framebuffer
    // (0,0 = top-left, 1,1 = bottom-right).
    void blit(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, int id,
              dm::box2 dstRect = dm::box2(dm::float2(0.f), dm::float2(1.f)));
    void endGraphics(nvrhi::ICommandList *commandList);
    void endGraphicsFrame();

 private:
    struct Target;
    nvrhi::DeviceHandle m_device;
    nvrhi::AutoPtr<donut::IGPAndNVRHIInteropDevice> m_interop;
    nvrhi::AutoPtr<donut::gp::IDevice> m_gpDevice;
    nvrhi::AutoPtr<donut::gp::IDeviceQueue> m_gpQueue;
    std::vector<std::unique_ptr<Target>> m_targets;
    nvrhi::GraphicsPipelineHandle m_pipeline;
    nvrhi::BindingLayoutHandle m_bindingLayout;
    nvrhi::SamplerHandle m_sampler;
};

}  // namespace SampleUtils

#endif /* SAMPLE_UTILS_SCREENPRESENTER_H */

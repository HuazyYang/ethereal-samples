#ifndef SRC_VXGISAMPLE_SAMPLERENDERTARGETS_H
#define SRC_VXGISAMPLE_SAMPLERENDERTARGETS_H
#include <nvrhi/nvrhi.h>
#include <donut/core/math/math.h>
#include <donut/engine/FramebufferFactory.h>
#include <nvrhi/core/autoptr.h>

class SampleRenderTargets {
public:
   SampleRenderTargets(nvrhi::IDevice *device);

   // Reentrant
   bool Init(dm::uint2 size);

   void ClearGBuffers(nvrhi::ICommandList *commandList);

   void CreateShadowFramebuffer(nvrhi::ITexture *shadowMapTexture);

   nvrhi::IDevice *Device;
   dm::uint2 Size;
   nvrhi::TextureHandle GBufferDepth;
   nvrhi::TextureHandle GBufferDiffuse;
   nvrhi::TextureHandle GBufferNormals;
   nvrhi::AutoPtr<donut::engine::FramebufferFactory> GBufferFramebuffer;

   nvrhi::TextureHandle DepthBuffer;
   nvrhi::TextureHandle HdrColorBuffer;
   nvrhi::AutoPtr<donut::engine::FramebufferFactory> HdrFramebuffer;
   nvrhi::TextureHandle ShadowMapTexture;
   nvrhi::AutoPtr<donut::engine::FramebufferFactory> ShadowMapFramebuffer;

   nvrhi::AutoPtr<donut::engine::FramebufferFactory> VoxelizationDummyFramebuffer;
};

#endif /* SRC_VXGISAMPLE_SAMPLERENDERTARGETS_H */

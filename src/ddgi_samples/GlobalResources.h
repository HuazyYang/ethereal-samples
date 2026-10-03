#include <nvrhi/nvrhi.h>
#include <donut/core/math/math.h>
#include <memory>
#include "ddgi/DDGIVolume.h"
#include "Types.h"

namespace donut::engine {
class DescriptorTableManager;
class TextureCache;
}

struct Config;

struct DDGIVolumeResources {
    nvrhi::TextureHandle rayDataTexture;
    nvrhi::TextureHandle probeIrradianceTexture;
    nvrhi::TextureHandle probeDistanceTexture;
    nvrhi::TextureHandle probeDataTexture;
    nvrhi::TextureHandle probeVariabilityTexture;
    nvrhi::TextureHandle probeVariabilityAverageTexture;
    nvrhi::StagingTextureHandle probeVariabilityReadbackBuffer;
};

class GlobalResources {
 public:
    nvrhi::IDevice *Device;
    dm::uint2 Size;

    Graphics::GlobalConstants GlobalConsts;
    Graphics::FrameConstants FrameConsts;

    // Common descriptor layout
    nvrhi::BindingLayoutHandle BindingLayout;
    nvrhi::BindingLayoutHandle BindlessLayout;
    nvrhi::BindingLayoutHandle FixedBindlessLayout;

    // Fixed binding set
    nvrhi::BindingSetHandle BindingSet;
    nvrhi::BindingSetHandle DDGIBindingSet;

    // Fixed descriptor heap
    nvrhi::DescriptorTableHandle FixedDescriptorTable;

    //  Fixed resources
    nvrhi::TextureHandle BlueNoiseTexture;

    // Fixed samplers
    nvrhi::SamplerHandle Samplers[3];

    // GBuffers
    nvrhi::TextureHandle GBufferA;
    nvrhi::TextureHandle GBufferB;
    nvrhi::TextureHandle GBufferC;
    nvrhi::TextureHandle GBufferD;

    // Path tracing resources
    nvrhi::TextureHandle PTOutputTexture;
    nvrhi::TextureHandle PTAccumulationTexture;

    // Scene Data
    nvrhi::DescriptorTableHandle SceneDescriptorTable;
    nvrhi::rt::AccelStructHandle SceneTLAS;
    nvrhi::BufferHandle SceneInstanceDataBuffer;
    nvrhi::BufferHandle SceneGeometryDataBuffer;
    nvrhi::BufferHandle SceneMaterialDataBuffer;
    nvrhi::BufferHandle SceneLightDataBuffer;

    // DDGI
    std::vector<ddgi::DDGIVolume> DDGIVolumes;
    std::vector<ddgi::DDGIVolumeDescGPUPacked> DDGIVolumeDescsPacked;
    std::vector<ddgi::DDGIVolumeResourceIndices> DDGIVolumeResourceIndices;
    nvrhi::TextureHandle DDGIOutputTexture;
    nvrhi::BufferHandle DDGIVolumeDescsBuffer;
    nvrhi::BufferHandle DDGIVolumeResourceIndicesBuffer;
    std::vector<DDGIVolumeResources> DDGIVolumesResources;

    // RTAO
    nvrhi::TextureHandle RTAOOutputTexture;
    nvrhi::TextureHandle RTAORawTexture;

    // Composite
    nvrhi::TextureHandle CompositeOutputTexture;
    nvrhi::FramebufferHandle CompositeFramebuffer;

    bool Initialize(nvrhi::IDevice *device, dm::uint2 size, dm::uint numFramesInFlight);

    void Update(const Config *config, const Graphics::FrameConstants &frameConsts);

    void Execute(nvrhi::ICommandList *commandList, dm::uint fbIndex);

    void Resize(dm::uint2 size);

    void SetBlueNoiseTexture(nvrhi::ITexture *blueNoiseTexture);

    void SetSceneBuffers(nvrhi::rt::IAccelStruct *bvh, nvrhi::IBuffer *instanceBuffer,
                         nvrhi::IBuffer *geometryBuffer, nvrhi::IBuffer *materialBuffer,
                         nvrhi::IBuffer *lightBuffer, nvrhi::IDescriptorTable *descriptorTable);

    void CreateBindingSets();

    void LoadDDGIVolumes(nvrhi::ICommandList *commandList, const Config *config);

    dm::uint GetNumVolumes();

    void SetGlobalConstsDirty();

    // nvrhi requires push constants to be set after every set*State call, even for passes that ignore them.

    void SetDefaultRootConstants(nvrhi::ICommandList* commandList) const;
 private:
    nvrhi::BufferHandle GlobalConstBuffer;
    nvrhi::BufferHandle FrameConstBuffer;
    nvrhi::BufferHandle DDGIVolumeDescsBufferUpload;
    nvrhi::BufferHandle DDGIVolumeResourceIndicesBufferUpload;
    dm::uint NumFramesInFlight;
    bool IsGlobalConstsDirty = true;

    // The passes track these textures explicitly: every frame they begin tracking each one in its steady
    // state (ShaderResource or UnorderedAccess) and leave it there. A texture is created in Common, the
    // state it really is in on every backend (on Vulkan the image starts in VK_IMAGE_LAYOUT_UNDEFINED
    // whatever TextureDesc::initialState says), and the next command list moves it to its steady state.
    std::vector<std::pair<nvrhi::TextureHandle, nvrhi::ResourceStates>> PendingInitialStates;
    void CreateTrackedTexture(nvrhi::TextureDesc desc, nvrhi::ResourceStates steadyState,
                              nvrhi::TextureHandle &texture);
    void CommitInitialStates(nvrhi::ICommandList *commandList);

    void CreateResizableResources(dm::uint2 size);
    void CreateBindingLayout();
    void CreateBindlessLayout();
    void CreateSamplers();
    void CreateConstantBuffers();
    void CreateFixedDescriptorTable();
    void CreateDDGIBindingSet();

    void CreateDDGIVolumeBuffers();
    void CreateDDGIVolumeTextures();
    void CreateDDGIOutputTexture();

    void ClearProbes(nvrhi::ICommandList *commandList, dm::uint volumeIndex);
};
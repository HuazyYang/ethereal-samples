#include "GlobalResources.h"
#include <donut/core/math/math.h>
#include <donut/engine/DescriptorTableManager.h>
#include <donut/engine/TextureCache.h>
#include "Config.h"

namespace {

enum DescriptorHeapOffsets {
    BLUE_NOISE_SRV_INDEX = 0,
    PT_OUTPUT_UAV_INDEX = 1,
    PT_ACCUMULATION_UAV_INDEX = 2,
    GBUFFERA_SRV_INDEX = 3,
    GBUFFERA_UAV_INDEX = 4,
    GBUFFERB_SRV_INDEX = 5,
    GBUFFERB_UAV_INDEX = 6,
    GBUFFERC_SRV_INDEX = 7,
    GBUFFERC_UAV_INDEX = 8,
    GBUFFERD_SRV_INDEX = 9,
    GBUFFERD_UAV_INDEX = 10,
    DDGI_OUTPUT_SRV_INDEX = 11,
    DDGI_OUTPUT_UAV_INDEX = 12,
    RTAO_OUTPUT_SRV_INDEX = 13,
    RTAO_OUTPUT_UAV_INDEX = 14,
    RTAO_RAW_SRV_INDEX = 15,
    RTAO_RAW_UAV_INDEX = 16,
    NUM_RESOURCE_DESCRIPTOR_OFFSETS = 17
};

static constexpr dm::uint DDGIMaxNumVolumes = 4;
static const dm::uint DDGIVolumeDescGPUSizeGranularity =
    dm::roundUp(sizeof(ddgi::DDGIVolumeDescGPUPacked) * DDGIMaxNumVolumes, 256u);
static const dm::uint DDGIVolumeResourceIndicesSizeGranularity =
    dm::roundUp(sizeof(ddgi::DDGIVolumeResourceIndices) * DDGIMaxNumVolumes, 256u);
}

static void GetDDGIVolumeDesc(
    const Config::Renderers::DDGI::DDGIVolume& config, dm::uint index, ddgi::DDGIVolumeDesc& desc) {
    desc.name = config.name;
    desc.index = index;
    desc.rngSeed = 0;
    desc.origin = config.origin;
    desc.eulerAngles = config.eulerAngles;
    desc.probeSpacing = config.probeSpacing;
    desc.probeCounts = dm::int3{config.probeCounts};
    desc.probeNumRays = config.probeNumRays;
    desc.probeNumIrradianceTexels = config.probeNumIrradianceTexels;
    desc.probeNumIrradianceInteriorTexels = config.probeNumIrradianceTexels - 2;
    desc.probeNumDistanceTexels = config.probeNumDistanceTexels;
    desc.probeNumDistanceInteriorTexels = config.probeNumDistanceTexels - 2;
    desc.probeHysteresis = config.probeHysteresis;
    desc.probeNormalBias = config.probeNormalBias;
    desc.probeViewBias = config.probeViewBias;
    desc.probeMaxRayDistance = config.probeMaxRayDistance;
    desc.probeIrradianceThreshold = config.probeIrradianceThreshold;
    desc.probeBrightnessThreshold = config.probeBrightnessThreshold;

    auto ConvertDDGITextureFormat = [](const char* fmtName) {
        if (strcmp(fmtName, "R10G10B10A2_UNORM") == 0)
            return ddgi::EDDGIVolumeTextureFormat::R10G10B2_UNORM;
        else if(strcmp(fmtName, "R16_FLOAT") == 0)
            return ddgi::EDDGIVolumeTextureFormat::F16;
        else if(strcmp(fmtName, "RG16_FLOAT") == 0)
            return ddgi::EDDGIVolumeTextureFormat::F16x2;
        else if(strcmp(fmtName, "RGBA16_FLOAT") == 0)
            return ddgi::EDDGIVolumeTextureFormat::F16x4;
        else if(strcmp(fmtName, "R32_FLOAT") == 0)
            return ddgi::EDDGIVolumeTextureFormat::F32;
        else if(strcmp(fmtName, "RG32_FLOAT") == 0)
            return ddgi::EDDGIVolumeTextureFormat::F32x2;
        else if(strcmp(fmtName, "RGBA32_FLOAT") == 0)
            return ddgi::EDDGIVolumeTextureFormat::F32x4;
        else
            assert(0);
        return ddgi::EDDGIVolumeTextureFormat::Count;
    };

    desc.probeRayDataFormat = ConvertDDGITextureFormat(config.texturesRayDataFormat.c_str());
    desc.probeIrradianceFormat =
        ConvertDDGITextureFormat(config.texturesIrradianceFormat.c_str());
    desc.probeDistanceFormat =
        ConvertDDGITextureFormat(config.texturesDistanceFormat.c_str());
    desc.probeDataFormat = ConvertDDGITextureFormat(config.texturesDataFormat.c_str());
    desc.probeVariabilityFormat =
        ConvertDDGITextureFormat(config.texturesVariabilityFormat.c_str());

    desc.probeRelocationEnabled = config.probeRelocationEnabled;
    desc.probeMinFrontfaceDistance = config.probeRelocationMinFrontfaceDistance;
    desc.probeClassificationEnabled = config.probeClassificationEnabled;
    desc.probeVariabilityEnabled = config.probeVariabilityEnabled;
    desc.probeVariabilityThreshold = config.probeVariabilityThreshold;

    if(config.infiniteScrollingEnabled)
        desc.movementType = ddgi::EDDGIVolumeMovementType::Scrolling;
    else
        desc.movementType = ddgi::EDDGIVolumeMovementType::Default;
}

nvrhi::Format GetDDGIVolumeTextureFormat(ddgi::EDDGIVolumeTextureFormat format) {
    switch (format) {
        case ddgi::EDDGIVolumeTextureFormat::R10G10B2_UNORM:
            return nvrhi::Format::R10G10B10A2_UNORM;
        case ddgi::EDDGIVolumeTextureFormat::F16:
            return nvrhi::Format::R16_FLOAT;
        case ddgi::EDDGIVolumeTextureFormat::F16x2:
            return nvrhi::Format::RG16_FLOAT;
        case ddgi::EDDGIVolumeTextureFormat::F16x4:
            return nvrhi::Format::RGBA16_FLOAT;
        case ddgi::EDDGIVolumeTextureFormat::F32:
            return nvrhi::Format::R32_FLOAT;
        case ddgi::EDDGIVolumeTextureFormat::F32x2:
            return nvrhi::Format::RG32_FLOAT;
        case ddgi::EDDGIVolumeTextureFormat::F32x4:
            return nvrhi::Format::RGBA32_FLOAT;
        default:
            assert(0);
            return nvrhi::Format::UNKNOWN;
    }
}

bool GlobalResources::Initialize(nvrhi::IDevice* device, dm::uint2 size,
                                 dm::uint numFramesInFlight) {
    Device = device;
    NumFramesInFlight = numFramesInFlight;
    CreateSamplers();
    CreateConstantBuffers();
    CreateBindingLayout();
    CreateBindlessLayout();
    CreateFixedDescriptorTable();
    CreateResizableResources(size);
    return true;
}

void GlobalResources::Update(const Config* config,
                             const Graphics::FrameConstants& frameConsts) {
    dm::uint numVolumes = GetNumVolumes();
    DDGIVolumeDescsPacked.resize(numVolumes);
    for (dm::uint index = 0; index < numVolumes; ++index) {
        DDGIVolumeDescsPacked[index] = DDGIVolumes[index].GetDescGPUPacked();
    }

    FrameConsts = frameConsts;
}

void GlobalResources::Execute(nvrhi::ICommandList* commandList, dm::uint fbIndex) {
    CommitInitialStates(commandList);

    if(IsGlobalConstsDirty) {
        commandList->beginTrackingBufferState(GlobalConstBuffer,
                                              nvrhi::ResourceStates::ConstantBuffer);
        commandList->setBufferState(GlobalConstBuffer, nvrhi::ResourceStates::CopyDest);
        commandList->writeBuffer(GlobalConstBuffer, &GlobalConsts, sizeof(GlobalConsts));
        commandList->setBufferState(GlobalConstBuffer,
                                    nvrhi::ResourceStates::ConstantBuffer);
        commandList->commitBarriers();
        IsGlobalConstsDirty = false;
    }

    commandList->writeBuffer(FrameConstBuffer, &FrameConsts, sizeof(FrameConsts));

    dm::uint numVolumes = GetNumVolumes();
    if (numVolumes == 0)
        return;

    // The destination buffers are written directly: nvrhi stages the data and transitions them to
    // CopyDest and back. Their state is tracked per command list, so start tracking explicitly.
    for (nvrhi::IBuffer* buffer : {DDGIVolumeDescsBuffer.Get(), DDGIVolumeResourceIndicesBuffer.Get()}) {
        commandList->beginTrackingBufferState(buffer, nvrhi::ResourceStates::ShaderResource);
    }
    commandList->writeBuffer(DDGIVolumeDescsBuffer, DDGIVolumeDescsPacked.data(),
                             sizeof(ddgi::DDGIVolumeDescGPUPacked) * numVolumes);
    commandList->writeBuffer(DDGIVolumeResourceIndicesBuffer, DDGIVolumeResourceIndices.data(),
                             sizeof(ddgi::DDGIVolumeResourceIndices) * numVolumes);
    commandList->setBufferState(DDGIVolumeDescsBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(DDGIVolumeResourceIndicesBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();
}

void GlobalResources::Resize(dm::uint2 size) {
    CreateResizableResources(size);
}

void GlobalResources::SetBlueNoiseTexture(nvrhi::ITexture* blueNoiseTexture) {
    BlueNoiseTexture = blueNoiseTexture;
    IsGlobalConstsDirty = true;
    Device->writeDescriptorTable(FixedDescriptorTable,
                                 nvrhi::BindingSetItem::Texture_SRV(0, BlueNoiseTexture));
}

void GlobalResources::SetSceneBuffers(nvrhi::rt::IAccelStruct* bvh,
                                      nvrhi::IBuffer* instanceBuffer,
                                      nvrhi::IBuffer* geometryBuffer,
                                      nvrhi::IBuffer* materialBuffer,
                                      nvrhi::IBuffer* lightBuffer,
                                      nvrhi::IDescriptorTable* descriptorTable) {
    SceneTLAS = bvh;
    SceneInstanceDataBuffer = instanceBuffer;
    SceneGeometryDataBuffer = geometryBuffer;
    SceneMaterialDataBuffer = materialBuffer;
    SceneLightDataBuffer = lightBuffer;
    SceneDescriptorTable = descriptorTable;
}

void GlobalResources::LoadDDGIVolumes(nvrhi::ICommandList *commandList, const Config* config) {
    dm::uint numVolumes = (int)config->renderers.ddgi.children.size();
    DDGIVolumes.clear();
    for (int index = 0; index < numVolumes; ++index) {
        ddgi::DDGIVolumeDesc desc;
        GetDDGIVolumeDesc(config->renderers.ddgi.children[index], (dm::uint)index, desc);
        DDGIVolumes.emplace_back(desc);
    }

    CreateDDGIVolumeBuffers();
    CreateDDGIVolumeTextures();
    CommitInitialStates(commandList);

    numVolumes = GetNumVolumes();
    for (dm::uint volumeIndex = 0; volumeIndex < numVolumes; ++volumeIndex)
        ClearProbes(commandList, volumeIndex);

    GlobalConsts.fixedRes.numDDGIVolumes = numVolumes;
    IsGlobalConstsDirty = true;
}

dm::uint GlobalResources::GetNumVolumes() {
    return std::min(DDGIMaxNumVolumes, (dm::uint)DDGIVolumes.size());
}

void GlobalResources::SetGlobalConstsDirty() { IsGlobalConstsDirty = true; }

void GlobalResources::SetDefaultRootConstants(nvrhi::ICommandList* commandList) const {
    Graphics::GlobalRootConstants constants = {};
    commandList->setPushConstants(&constants, sizeof(constants));
}

void GlobalResources::CreateBindingSets() {
    nvrhi::BindingSetDesc desc;
    desc.bindings = {
        nvrhi::BindingSetItem::PushConstants(0, sizeof(Graphics::GlobalRootConstants)),
        nvrhi::BindingSetItem::ConstantBuffer(1, FrameConstBuffer),
        nvrhi::BindingSetItem::ConstantBuffer(2, GlobalConstBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(0, SceneLightDataBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(1, SceneMaterialDataBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(2, SceneInstanceDataBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(3, SceneGeometryDataBuffer),
        nvrhi::BindingSetItem::RayTracingAccelStruct(4, SceneTLAS),
        // nvrhi requires a binding set to provide every item of its layout.
        nvrhi::BindingSetItem::StructuredBuffer_SRV(5, DDGIVolumeDescsBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(6, DDGIVolumeResourceIndicesBuffer),
        // Descriptors.hlsli declares "SamplerState Samplers[3] : register(s0)": one 3-element array.
        nvrhi::BindingSetItem::Sampler(0, Samplers[0]).setArrayElement(0),
        nvrhi::BindingSetItem::Sampler(0, Samplers[1]).setArrayElement(1),
        nvrhi::BindingSetItem::Sampler(0, Samplers[2]).setArrayElement(2)};

    Device->createBindingSet(desc, BindingLayout, &BindingSet);

    desc.bindings = {
        nvrhi::BindingSetItem::PushConstants(0, sizeof(Graphics::GlobalRootConstants)),
        nvrhi::BindingSetItem::ConstantBuffer(1, FrameConstBuffer),
        nvrhi::BindingSetItem::ConstantBuffer(2, GlobalConstBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(0, SceneLightDataBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(1, SceneMaterialDataBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(2, SceneInstanceDataBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(3, SceneGeometryDataBuffer),
        nvrhi::BindingSetItem::RayTracingAccelStruct(4, SceneTLAS),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(5, DDGIVolumeDescsBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(6, DDGIVolumeResourceIndicesBuffer),
        // Descriptors.hlsli declares "SamplerState Samplers[3] : register(s0)": one 3-element array.
        nvrhi::BindingSetItem::Sampler(0, Samplers[0]).setArrayElement(0),
        nvrhi::BindingSetItem::Sampler(0, Samplers[1]).setArrayElement(1),
        nvrhi::BindingSetItem::Sampler(0, Samplers[2]).setArrayElement(2)};

    Device->createBindingSet(desc, BindingLayout, &DDGIBindingSet);
}

void GlobalResources::CreateDDGIVolumeBuffers() {
    dm::uint numVolumes = GetNumVolumes();
    nvrhi::BufferDesc buffDesc;
    buffDesc.byteSize = numVolumes * sizeof(ddgi::DDGIVolumeDescGPUPacked);
    buffDesc.structStride = sizeof(ddgi::DDGIVolumeDescGPUPacked);
    buffDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    buffDesc.keepInitialState = false;  // written every frame, so its state must be tracked
    buffDesc.canHaveTypedViews = true;
    buffDesc.debugName = "DDGIVolumeDescsBuffer";
    Device->createBuffer(buffDesc, &DDGIVolumeDescsBuffer);

    buffDesc.byteSize = numVolumes * sizeof(ddgi::DDGIVolumeResourceIndices);
    buffDesc.structStride = sizeof(ddgi::DDGIVolumeResourceIndices);
    buffDesc.debugName = "DDGIVolumeResourceIndicesBuffer";
    Device->createBuffer(buffDesc, &DDGIVolumeResourceIndicesBuffer);
}

void GlobalResources::CreateDDGIVolumeTextures() {
    dm::uint numVolumes;
    nvrhi::Format format;
    dm::uint width, height, arraySize;
    nvrhi::TextureDesc texDesc;
    dm::uint descriptorHeapCapacity;
    dm::uint descriptorOffset;
    dm::uint descriptorIndex;

    numVolumes = GetNumVolumes();

    descriptorHeapCapacity = FixedDescriptorTable->getCapacity();
    if (descriptorHeapCapacity < NUM_RESOURCE_DESCRIPTOR_OFFSETS + numVolumes * 6 * 2) {
        descriptorHeapCapacity = NUM_RESOURCE_DESCRIPTOR_OFFSETS + numVolumes * 6 * 2;

        Device->resizeDescriptorTable(FixedDescriptorTable, descriptorHeapCapacity);
        GlobalConsts.fixedRes.fixedResourceIndexOffset =
            FixedDescriptorTable->getFirstDescriptorIndexInHeap();
        IsGlobalConstsDirty = true;
    }

    texDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
    texDesc.isUAV = true;

    DDGIVolumeResourceIndices.resize(numVolumes);
    DDGIVolumesResources.resize(numVolumes);

    descriptorOffset = FixedDescriptorTable->getFirstDescriptorIndexInHeap();
    descriptorIndex = NUM_RESOURCE_DESCRIPTOR_OFFSETS;

    for (dm::uint index = 0; index < numVolumes; ++index) {
        auto& desc = DDGIVolumes[index].GetDesc();
        auto& resourceIndices = DDGIVolumeResourceIndices[index];
        auto& resources = DDGIVolumesResources[index];

        std::string debugNamePrefix = "DDGIVolume[" + std::to_string(index) + "].";

        // Probe ray data texture
        {
            ddgi::GetDDGIVolumeTextureDimensions(
                desc, ddgi::EDDGIVolumeTextureType::RayData, width, height, arraySize);
            format = GetDDGIVolumeTextureFormat(desc.probeRayDataFormat);

            texDesc.width = width;
            texDesc.height = height;
            texDesc.arraySize = arraySize;
            texDesc.format = format;
            texDesc.debugName = debugNamePrefix + "probeRayData";

            CreateTrackedTexture(texDesc, nvrhi::ResourceStates::UnorderedAccess, resources.rayDataTexture);
            Device->writeDescriptorTable(FixedDescriptorTable,
                                         nvrhi::BindingSetItem::Texture_SRV(
                                             descriptorIndex, resources.rayDataTexture));
            resourceIndices.rayDataSRVIndex = descriptorOffset  + descriptorIndex++;
            Device->writeDescriptorTable(FixedDescriptorTable,
                                         nvrhi::BindingSetItem::Texture_UAV(
                                             descriptorIndex, resources.rayDataTexture));
            resourceIndices.rayDataUAVIndex = descriptorOffset + descriptorIndex++;
        }

        // Probe irradiance texture
        {
            ddgi::GetDDGIVolumeTextureDimensions(
                desc, ddgi::EDDGIVolumeTextureType::Irradiance, width, height, arraySize);
            format = GetDDGIVolumeTextureFormat(desc.probeIrradianceFormat);

            texDesc.width = width;
            texDesc.height = height;
            texDesc.arraySize = arraySize;
            texDesc.format = format;
            texDesc.debugName = debugNamePrefix + "probeIrradiance";

            CreateTrackedTexture(texDesc, nvrhi::ResourceStates::UnorderedAccess, resources.probeIrradianceTexture);
            Device->writeDescriptorTable(FixedDescriptorTable,
                                         nvrhi::BindingSetItem::Texture_SRV(
                                             descriptorIndex, resources.probeIrradianceTexture));
            resourceIndices.probeIrradianceSRVIndex = descriptorOffset + descriptorIndex++;
            Device->writeDescriptorTable(
                FixedDescriptorTable,
                nvrhi::BindingSetItem::Texture_UAV(descriptorIndex,
                                                   resources.probeIrradianceTexture));
            resourceIndices.probeIrradianceUAVIndex = descriptorOffset + descriptorIndex++;
        }

        // Probe distance texture
        {
            ddgi::GetDDGIVolumeTextureDimensions(
                desc, ddgi::EDDGIVolumeTextureType::Distance, width, height, arraySize);
            format = GetDDGIVolumeTextureFormat(desc.probeDistanceFormat);

            texDesc.width = width;
            texDesc.height = height;
            texDesc.arraySize = arraySize;
            texDesc.format = format;
            texDesc.debugName = debugNamePrefix + "probeDistance";

            CreateTrackedTexture(texDesc, nvrhi::ResourceStates::UnorderedAccess, resources.probeDistanceTexture);
            Device->writeDescriptorTable(
                FixedDescriptorTable, nvrhi::BindingSetItem::Texture_SRV(
                                          descriptorIndex, resources.probeDistanceTexture));
            resourceIndices.probeDistanceSRVIndex = descriptorOffset + descriptorIndex++;
            Device->writeDescriptorTable(
                FixedDescriptorTable, nvrhi::BindingSetItem::Texture_UAV(
                                          descriptorIndex, resources.probeDistanceTexture));
            resourceIndices.probeDistanceUAVIndex = descriptorOffset + descriptorIndex++;
        }

        // Probe data texture
        {
            ddgi::GetDDGIVolumeTextureDimensions(desc, ddgi::EDDGIVolumeTextureType::Data,
                                                 width, height, arraySize);
            format = GetDDGIVolumeTextureFormat(desc.probeDataFormat);

            texDesc.width = width;
            texDesc.height = height;
            texDesc.arraySize = arraySize;
            texDesc.format = format;
            texDesc.debugName = debugNamePrefix + "probeData";

            CreateTrackedTexture(texDesc, nvrhi::ResourceStates::UnorderedAccess, resources.probeDataTexture);
            Device->writeDescriptorTable(FixedDescriptorTable,
                                         nvrhi::BindingSetItem::Texture_SRV(
                                             descriptorIndex, resources.probeDataTexture));
            resourceIndices.probeDataSRVIndex = descriptorOffset + descriptorIndex++;
            Device->writeDescriptorTable(FixedDescriptorTable,
                                         nvrhi::BindingSetItem::Texture_UAV(
                                             descriptorIndex, resources.probeDataTexture));
            resourceIndices.probeDataUAVIndex = descriptorOffset + descriptorIndex++;
        }

        // Probe variability texture
        {
            ddgi::GetDDGIVolumeTextureDimensions(
                desc, ddgi::EDDGIVolumeTextureType::Variability, width, height, arraySize);
            format = GetDDGIVolumeTextureFormat(desc.probeVariabilityFormat);

            texDesc.width = width;
            texDesc.height = height;
            texDesc.arraySize = arraySize;
            texDesc.format = format;
            texDesc.debugName = debugNamePrefix + "probeVariability";

            CreateTrackedTexture(texDesc, nvrhi::ResourceStates::UnorderedAccess, resources.probeVariabilityTexture);
            Device->writeDescriptorTable(
                FixedDescriptorTable,
                nvrhi::BindingSetItem::Texture_SRV(descriptorIndex,
                                                   resources.probeVariabilityTexture));
            resourceIndices.probeVariabilitySRVIndex = descriptorOffset + descriptorIndex++;
            Device->writeDescriptorTable(
                FixedDescriptorTable,
                nvrhi::BindingSetItem::Texture_UAV(descriptorIndex,
                                                   resources.probeVariabilityTexture));
            resourceIndices.probeVariabilityUAVIndex = descriptorOffset + descriptorIndex++;
        }

        // Probe variability average
        {
            ddgi::GetDDGIVolumeTextureDimensions(
                desc, ddgi::EDDGIVolumeTextureType::VariabilityAverage, width, height,
                arraySize);
            format = nvrhi::Format::RG32_FLOAT;

            texDesc.width = width;
            texDesc.height = height;
            texDesc.arraySize = arraySize;
            texDesc.format = format;
            texDesc.debugName = debugNamePrefix + "probeVariabilityAverage";

            CreateTrackedTexture(texDesc, nvrhi::ResourceStates::UnorderedAccess, resources.probeVariabilityAverageTexture);
            Device->writeDescriptorTable(
                FixedDescriptorTable,
                nvrhi::BindingSetItem::Texture_SRV(
                    descriptorIndex, resources.probeVariabilityAverageTexture));
            resourceIndices.probeVariabilityAverageSRVIndex =
                descriptorOffset + descriptorIndex++;
            Device->writeDescriptorTable(
                FixedDescriptorTable,
                nvrhi::BindingSetItem::Texture_UAV(
                    descriptorIndex, resources.probeVariabilityAverageTexture));
            resourceIndices.probeVariabilityAverageUAVIndex =
                descriptorOffset + descriptorIndex++;

            nvrhi::TextureDesc buffDesc;
            buffDesc.width = 1;
            buffDesc.height = 1;
            buffDesc.format = nvrhi::Format::RG32_FLOAT;
            buffDesc.debugName = debugNamePrefix + "probeVariabilityReadback";
            Device->createStagingTexture(buffDesc, nvrhi::CpuAccessMode::Read, &resources.probeVariabilityReadbackBuffer);
        }
    }
}

void GlobalResources::ClearProbes(nvrhi::ICommandList* commandList, dm::uint volumeIndex) {
    auto& resources = DDGIVolumesResources[volumeIndex];
    commandList->beginTrackingTextureState(resources.probeIrradianceTexture,
                                           nvrhi::AllSubresources,
                                           nvrhi::ResourceStates::UnorderedAccess);
    commandList->clearTextureFloat(resources.probeIrradianceTexture, nvrhi::AllSubresources,
                                   nvrhi::Color{0.f});

    commandList->beginTrackingTextureState(resources.probeDistanceTexture,
                                           nvrhi::AllSubresources,
                                           nvrhi::ResourceStates::UnorderedAccess);
    commandList->clearTextureFloat(resources.probeDistanceTexture, nvrhi::AllSubresources,
                                   nvrhi::Color{0.f});
    DDGIVolumes[volumeIndex].ResetProbeVariability();
}

void GlobalResources::CreateResizableResources(dm::uint2 size) {
    Size = size;

    nvrhi::TextureDesc desc;
    desc.width = size.x;
    desc.height = size.y;
    desc.isUAV = true;
    desc.isRenderTarget = false;
    desc.keepInitialState = false;

    // GBuffers
    {
        desc.format = nvrhi::Format::RGBA8_UNORM;
        desc.debugName = "GBufferA";
        CreateTrackedTexture(desc, nvrhi::ResourceStates::ShaderResource, GBufferA);
        Device->writeDescriptorTable(
            FixedDescriptorTable,
            nvrhi::BindingSetItem::Texture_SRV(GBUFFERA_SRV_INDEX, GBufferA));
        Device->writeDescriptorTable(
            FixedDescriptorTable,
            nvrhi::BindingSetItem::Texture_UAV(GBUFFERA_UAV_INDEX, GBufferA));

        desc.format = nvrhi::Format::RGBA32_FLOAT;
        desc.debugName = "GBufferB";
        CreateTrackedTexture(desc, nvrhi::ResourceStates::ShaderResource, GBufferB);
        Device->writeDescriptorTable(
            FixedDescriptorTable,
            nvrhi::BindingSetItem::Texture_SRV(GBUFFERB_SRV_INDEX, GBufferB));
        Device->writeDescriptorTable(
            FixedDescriptorTable,
            nvrhi::BindingSetItem::Texture_UAV(GBUFFERB_UAV_INDEX, GBufferB));

        desc.format = nvrhi::Format::RGBA32_FLOAT;
        desc.debugName = "GBufferC";
        CreateTrackedTexture(desc, nvrhi::ResourceStates::ShaderResource, GBufferC);
        Device->writeDescriptorTable(
            FixedDescriptorTable,
            nvrhi::BindingSetItem::Texture_SRV(GBUFFERC_SRV_INDEX, GBufferC));
        Device->writeDescriptorTable(
            FixedDescriptorTable,
            nvrhi::BindingSetItem::Texture_UAV(GBUFFERC_UAV_INDEX, GBufferC));

        desc.format = nvrhi::Format::RGBA32_FLOAT;
        desc.debugName = "GBufferD";
        CreateTrackedTexture(desc, nvrhi::ResourceStates::ShaderResource, GBufferD);
        Device->writeDescriptorTable(
            FixedDescriptorTable,
            nvrhi::BindingSetItem::Texture_SRV(GBUFFERD_SRV_INDEX, GBufferD));
        Device->writeDescriptorTable(
            FixedDescriptorTable,
            nvrhi::BindingSetItem::Texture_UAV(GBUFFERD_UAV_INDEX, GBufferD));
    }

    // Path tracing
    {
        desc.format = nvrhi::Format::RGBA8_UNORM;
        desc.debugName = "PTOutputTexture";
        CreateTrackedTexture(desc, nvrhi::ResourceStates::ShaderResource, PTOutputTexture);
        Device->writeDescriptorTable(
            FixedDescriptorTable,
            nvrhi::BindingSetItem::Texture_UAV(PT_OUTPUT_UAV_INDEX, PTOutputTexture));

        desc.format = nvrhi::Format::RGBA32_FLOAT;
        desc.debugName = "PTAccumulationTexture";
        // Only ever accessed through its bindless UAV descriptor, never transitioned by a pass.
        CreateTrackedTexture(desc, nvrhi::ResourceStates::UnorderedAccess, PTAccumulationTexture);
        Device->writeDescriptorTable(FixedDescriptorTable,
                                     nvrhi::BindingSetItem::Texture_UAV(
                                         PT_ACCUMULATION_UAV_INDEX, PTAccumulationTexture));
    }

    // Create DDGI output texture
    {
        // DDGIRenderPass and RTAOPass begin tracking these textures as ShaderResource every frame and leave
        // them there, so that is their steady state.
        desc.format = nvrhi::Format::RGBA16_FLOAT;
        desc.debugName = "DDGIOuputTexture";
        CreateTrackedTexture(desc, nvrhi::ResourceStates::ShaderResource, DDGIOutputTexture);
        Device->writeDescriptorTable(
            FixedDescriptorTable,
            nvrhi::BindingSetItem::Texture_SRV(DDGI_OUTPUT_SRV_INDEX, DDGIOutputTexture));
        Device->writeDescriptorTable(
            FixedDescriptorTable,
            nvrhi::BindingSetItem::Texture_UAV(DDGI_OUTPUT_UAV_INDEX, DDGIOutputTexture));
    }

    // RTAO output textures
    {
        desc.format = nvrhi::Format::R8_UNORM;
        desc.debugName = "RTAO Output";
        CreateTrackedTexture(desc, nvrhi::ResourceStates::ShaderResource, RTAOOutputTexture);
        Device->writeDescriptorTable(
            FixedDescriptorTable,
            nvrhi::BindingSetItem::Texture_SRV(RTAO_OUTPUT_SRV_INDEX, RTAOOutputTexture));
        Device->writeDescriptorTable(
            FixedDescriptorTable,
            nvrhi::BindingSetItem::Texture_UAV(RTAO_OUTPUT_UAV_INDEX, RTAOOutputTexture));

        desc.debugName = "RTAO Raw";
        CreateTrackedTexture(desc, nvrhi::ResourceStates::ShaderResource, RTAORawTexture);
        Device->writeDescriptorTable(
            FixedDescriptorTable,
            nvrhi::BindingSetItem::Texture_SRV(RTAO_RAW_SRV_INDEX, RTAORawTexture));
        Device->writeDescriptorTable(
            FixedDescriptorTable,
            nvrhi::BindingSetItem::Texture_UAV(RTAO_RAW_UAV_INDEX, RTAORawTexture));
    }

    // Composite
    {
        desc.format = nvrhi::Format::RGBA8_UNORM;
        desc.isRenderTarget = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName = "CompositeOutputTexture";
        Device->createTexture(desc, &CompositeOutputTexture);

        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(CompositeOutputTexture);
        Device->createFramebuffer(fbDesc, &CompositeFramebuffer);
    }
}

void GlobalResources::CreateTrackedTexture(nvrhi::TextureDesc desc, nvrhi::ResourceStates steadyState,
                                           nvrhi::TextureHandle& texture) {
    desc.initialState = nvrhi::ResourceStates::Common;
    desc.keepInitialState = false;
    texture = nullptr;
    Device->createTexture(desc, &texture);
    PendingInitialStates.emplace_back(texture, steadyState);
}

void GlobalResources::CommitInitialStates(nvrhi::ICommandList* commandList) {
    if (PendingInitialStates.empty())
        return;

    for (auto& [texture, state] : PendingInitialStates) {
        commandList->beginTrackingTextureState(texture, nvrhi::AllSubresources,
                                               nvrhi::ResourceStates::Common);
        commandList->setTextureState(texture, nvrhi::AllSubresources, state);
    }
    commandList->commitBarriers();
    PendingInitialStates.clear();
}

void GlobalResources::CreateBindingLayout() {
    nvrhi::BindingLayoutDesc globalBindingLayoutDesc;
    globalBindingLayoutDesc.visibility = nvrhi::ShaderType::All;
    globalBindingLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::PushConstants(
            0, sizeof(Graphics::GlobalRootConstants)),        // GlobalRootConst
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(1),  // FrameConst
        nvrhi::BindingLayoutItem::ConstantBuffer(2),          // GlobalConst
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),    // Lights
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),    // Materials
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),    // TLASInstances
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),    // BLASInstances
        nvrhi::BindingLayoutItem::RayTracingAccelStruct(4),   // SceneBVH
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5),    // DDGIVolumes
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6),    // DDGIVolumeBindless

        // "SamplerState Samplers[3] : register(s0)" is a single array binding. D3D flattens it to s0..s2
        // either way, but on Vulkan it is one binding with descriptorCount 3, not three bindings of 1.
        nvrhi::BindingLayoutItem::Sampler(0).setSize(3),
    };
    Device->createBindingLayout(globalBindingLayoutDesc, &BindingLayout);
}

void GlobalResources::CreateBindlessLayout() {
    nvrhi::BindlessLayoutDesc bindlessLayoutDesc;
    bindlessLayoutDesc.visibility = nvrhi::ShaderType::All;
    bindlessLayoutDesc.firstSlot = 0;
    bindlessLayoutDesc.maxCapacity = 1024;
    bindlessLayoutDesc.registerSpaces = {nvrhi::BindingLayoutItem::RawBuffer_SRV(1),
                                         nvrhi::BindingLayoutItem::Texture_SRV(2)};
    Device->createBindlessLayout(bindlessLayoutDesc, &BindlessLayout);

    if (Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12) {
        bindlessLayoutDesc.registerSpaces = {nvrhi::BindingLayoutItem::Texture_SRV(3),
                                             nvrhi::BindingLayoutItem::Texture_SRV(4),
                                             nvrhi::BindingLayoutItem::Texture_UAV(1),
                                             nvrhi::BindingLayoutItem::Texture_UAV(2)};
    } else {
        bindlessLayoutDesc.registerSpaces = {nvrhi::BindingLayoutItem::Texture_SRV(0),
                                             nvrhi::BindingLayoutItem::Texture_UAV(1)};
    }
    Device->createBindlessLayout(bindlessLayoutDesc, &FixedBindlessLayout);
}

void GlobalResources::CreateSamplers() {
    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.minFilter = true;
    samplerDesc.magFilter = true;
    samplerDesc.mipFilter = false;
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    Device->createSampler(samplerDesc, &Samplers[0]);

    samplerDesc.minFilter = false;
    samplerDesc.magFilter = false;
    samplerDesc.mipFilter = false;
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    Device->createSampler(samplerDesc, &Samplers[1]);

    samplerDesc.setAllFilters(true)
        .setAllAddressModes(nvrhi::SamplerAddressMode::Wrap)
        .setMaxAnisotropy(16.f);
    Device->createSampler(samplerDesc, &Samplers[2]);
}

void GlobalResources::CreateConstantBuffers() {
    nvrhi::BufferDesc bufDesc;
    bufDesc.isConstantBuffer = true;
    bufDesc.isVolatile = false;
    bufDesc.byteSize = sizeof(Graphics::GlobalConstants);
    bufDesc.maxVersions = 0;
    bufDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    bufDesc.debugName = "GlobalConstBuffer";
    Device->createBuffer(bufDesc, &GlobalConstBuffer);

    bufDesc.isConstantBuffer = true;
    bufDesc.isVolatile = true;
    // nvrhi requires volatile constant buffers to have no CPU access: write-discard is implied.
    bufDesc.cpuAccess = nvrhi::CpuAccessMode::None;
    bufDesc.byteSize = sizeof(Graphics::FrameConstants);
    bufDesc.maxVersions = NumFramesInFlight;
    bufDesc.debugName = "FrameConstBuffer";
    Device->createBuffer(bufDesc, &FrameConstBuffer);

    // DDGI Upload buffer
    {
        bufDesc.isConstantBuffer = false;
        bufDesc.isVolatile = false;
        bufDesc.cpuAccess = nvrhi::CpuAccessMode::Write;
        bufDesc.byteSize = DDGIVolumeDescGPUSizeGranularity * NumFramesInFlight;
        bufDesc.structStride = 0;
        bufDesc.canHaveTypedViews = false;
        bufDesc.initialState = nvrhi::ResourceStates::CopySource;

        bufDesc.debugName = "DDGIVolumeDescsBufferUpload";
        Device->createBuffer(bufDesc, &DDGIVolumeDescsBufferUpload);

        bufDesc.byteSize = DDGIVolumeResourceIndicesSizeGranularity * NumFramesInFlight;
        bufDesc.debugName = "DDGIVolumeResourceIndicesBufferUpload";
        Device->createBuffer(bufDesc, &DDGIVolumeResourceIndicesBufferUpload);
    }
}

void GlobalResources::CreateFixedDescriptorTable() {
    Device->createDescriptorTable(FixedBindlessLayout, &FixedDescriptorTable);
    Device->resizeDescriptorTable(FixedDescriptorTable, NUM_RESOURCE_DESCRIPTOR_OFFSETS + 6 * 2 * DDGIMaxNumVolumes);

    GlobalConsts.fixedRes.fixedResourceIndexOffset =
        FixedDescriptorTable->getFirstDescriptorIndexInHeap();
    IsGlobalConstsDirty = true;
}

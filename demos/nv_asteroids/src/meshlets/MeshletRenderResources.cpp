#include "meshlets/MeshletRenderResources.h"
#include "meshlets/MeshletShaderTypes.h"
#include "meshlets/MeshShaderMode.h"

#include <nvrhi/utils.h>

namespace
{
    // The 2018 constant buffers were all volatile (BufferDesc +21 set, checked by the binding code to pick the
    // volatile CBV binding type). The version count only matters for Vulkan.
    constexpr uint32_t c_MaxConstantBufferVersions = 16;

    nvrhi::BufferHandle CreateVolatileConstants(nvrhi::IDevice* device, uint32_t byteSize, const char* name)
    {
        nvrhi::BufferHandle result;
        device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(byteSize, name, c_MaxConstantBufferVersions), &result);
        return result;
    }

    nvrhi::BufferHandle CreateUavBuffer(nvrhi::IDevice* device, uint64_t byteSize, uint32_t structStride, const char* name)
    {
        // deviation: the 2018 buffers only set canHaveUAVs and an initial state; donut's nvrhi also needs the
        // structure stride for the structured UAV view (u_Stats is RWStructuredBuffer<uint>).
        nvrhi::BufferDesc desc;
        desc.byteSize = byteSize;
        desc.structStride = structStride;
        desc.canHaveUAVs = true;
        desc.debugName = name;
        desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        desc.keepInitialState = true;
        nvrhi::BufferHandle result;
        device->createBuffer(desc, &result);
        return result;
    }
}

MeshletRenderResources::MeshletRenderResources(nvrhi::IDevice* device)
{
    // Asteroids.exe: 0x140040680
    frameConstants = CreateVolatileConstants(device, sizeof(meshlet_shader::FrameCB), "FrameRendererConstants");
    meshletInfoConstants = CreateVolatileConstants(device, sizeof(meshlet_shader::MeshletInfoCB), "MeshletInfoConstants");
    instanceConstants = CreateVolatileConstants(device, sizeof(meshlet_shader::InstanceCB), "InstanceConstants");
    instanceConstantsPrevious = CreateVolatileConstants(device, sizeof(meshlet_shader::InstanceCB), "InstanceConstantsPrevious");
    sectorConstants = CreateVolatileConstants(device, sizeof(meshlet_shader::SectorInfo), "SectorConstants");
    objectConstants = CreateVolatileConstants(device, uint32_t(meshlet_shader::c_ObjectConstantsUploadSize), "ObjectConstants");

    debugUAV = CreateUavBuffer(device, 0x10000 * 20, 20, "DebugUAV");
    statsUAV = CreateUavBuffer(device, 16 * sizeof(uint32_t), sizeof(uint32_t), "StatsUAV");

    // 2018 sampler desc: clamp addressing, mip bias 0, anisotropy 1, min/mag/mip linear, border colour 1.
    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp)
        .setAllFilters(true)
        .setMaxAnisotropy(1.f)
        .setBorderColor(nvrhi::Color(1.f));
    device->createSampler(samplerDesc, &linearClampSampler);

    // Same desc with reduction type 3 (maximum).
    samplerDesc.setReductionType(nvrhi::SamplerReductionType::Maximum);
    device->createSampler(samplerDesc, &maxReductionSampler);

    nvrhi::TextureDesc nullDesc;
    nullDesc.setWidth(1).setHeight(1).setFormat(nvrhi::Format::R32_FLOAT).setDebugName("MeshletNullTexture")
        .setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true);
    device->createTexture(nullDesc, &nullTexture);
    nvrhi::CommandListHandle commandList;
    device->createCommandList(nvrhi::CommandListParameters(), &commandList);
    commandList->open();
    const float zero = 0.f;
    commandList->writeTexture(nullTexture, 0, 0, &zero, sizeof(zero));
    commandList->close();
    device->executeCommandList(commandList);

    if (GetMeshShaderMode() == MeshShaderMode::Nvapi)
    {
        nvmesh2018::CreateExtensionBinding(device, 7, 0, nvExtensionBuffer, nvExtensionBindingLayout, nvExtensionBindingSet);
    }
}

bool nvmesh2018::CreateExtensionBinding(nvrhi::IDevice* device, uint32_t slot, uint32_t registerSpace,
    nvrhi::BufferHandle& buffer, nvrhi::BindingLayoutHandle& layout, nvrhi::BindingSetHandle& set)
{
    buffer = CreateUavBuffer(device, c_ShaderExtnStructSize, c_ShaderExtnStructSize, "NvShaderExtnUAV");
    if (!buffer)
        return false;

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(slot, buffer));
    return nvrhi::utils::CreateBindingSetAndLayout(device, nvrhi::ShaderType::All, registerSpace, setDesc, layout, set);
}

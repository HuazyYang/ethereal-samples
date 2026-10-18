// SPDX-License-Identifier: Apache-2.0
#include "GVDBSlangRayTracing.h"
#include "ComputeRuntime.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
using namespace nvrhi;
namespace gvdb::slang
{
namespace
{
void check(FRESULT status, const char *operation)
{
    if (NVRHI_FAILED(status))
        throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(status) + ")");
}
void finite(float value)
{
    if (!std::isfinite(value))
        throw std::invalid_argument("GVDB RT requires finite parameters");
}
void validate(const RaycastParameters &p, const std::vector<Float4> &transfer, const rt::AffineTransform &transform)
{
    validateRaycastParameters(p, transfer);
    if (p.march.y < 0 || p.march.y >= p.march.z)
        throw std::invalid_argument("GVDB RT requires a nonnegative, nonempty ray interval");
    for (float value : transform)
        finite(value);
    const float determinant = transform[0] * (transform[5] * transform[10] - transform[6] * transform[9]) -
                              transform[1] * (transform[4] * transform[10] - transform[6] * transform[8]) +
                              transform[2] * (transform[4] * transform[9] - transform[5] * transform[8]);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12f)
        throw std::invalid_argument("GVDB RT transform must be invertible");
}
} // namespace
SlangVolumeRT::SlangVolumeRT(ethereal::slang::ComputeRuntime &runtime, SlangVolume &volume,
                             const std::filesystem::path &artifact)
    : m_runtime(&runtime), m_volume(&volume)
{
    auto device = runtime.device();
    if (!device->queryFeatureSupport(Feature::RayTracingPipeline))
        throw std::runtime_error("Ray tracing pipelines are unavailable");
    std::ifstream file(artifact, std::ios::binary);
    if (!file)
        throw std::runtime_error("Missing GVDB RT artifact: " + artifact.string());
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    check(createShaderProgram(device, bytes.data(), bytes.size(), &m_program), "create RT program");
    std::map<std::string, ShaderHandle> shaders;
    for (uint32_t i = 0; i < m_program->getEntryPointCount(); ++i)
    {
        ShaderEntryPointDesc entry;
        check(m_program->getEntryPointDesc(i, entry), "get RT entry");
        check(m_program->createShader(i, &shaders[entry.name.c_str()]), "create RT shader");
        BindingLayoutDesc desc;
        check(m_program->getBindingLayoutDesc(i, 0, desc), "reflect RT bindings");
        if (i == 0)
            m_layoutDesc = desc;
        else
            for (const auto &item : desc.bindings)
            {
                const auto found =
                    std::find_if(m_layoutDesc.bindings.begin(), m_layoutDesc.bindings.end(),
                                 [&](const auto &other) { return item.slot == other.slot && item.type == other.type; });
                if (found == m_layoutDesc.bindings.end())
                    m_layoutDesc.bindings.push_back(item);
            }
    }
    m_layoutDesc.visibility = ShaderType::AllRayTracing;
    check(device->createBindingLayout(m_layoutDesc, &m_layout), "create RT binding layout");
    auto shader = [&](const char *name) -> IShader * {
        auto found = shaders.find(name);
        if (found == shaders.end() || !found->second)
            throw std::runtime_error(std::string("Missing GVDB RT entry: ") + name);
        return found->second;
    };
    rt::PipelineDesc pipeline;
    pipeline.setMaxPayloadSize(32)
        .setMaxAttributeSize(16)
        .setMaxRecursionDepth(2)
        .addBindingLayout(m_layout)
        .addShader(rt::PipelineShaderDesc().setExportName("rayGeneration").setShader(shader("rayGeneration")))
        .addShader(rt::PipelineShaderDesc().setExportName("radianceMiss").setShader(shader("radianceMiss")))
        .addShader(rt::PipelineShaderDesc().setExportName("shadowMiss").setShader(shader("shadowMiss")));
    for (const char *mode : {"volume", "surface"})
    {
        const std::string prefix(mode);
        pipeline.addHitGroup(rt::PipelineHitGroupDesc()
                                 .setExportName((prefix + "Radiance").c_str())
                                 .setIntersectionShader(shader((prefix + "Intersection").c_str()))
                                 .setClosestHitShader(shader((prefix + "ClosestHit").c_str()))
                                 .setIsProceduralPrimitive(true));
        pipeline.addHitGroup(rt::PipelineHitGroupDesc()
                                 .setExportName((prefix + "Shadow").c_str())
                                 .setIntersectionShader(shader((prefix + "Intersection").c_str()))
                                 .setAnyHitShader(shader((prefix + "ShadowAnyHit").c_str()))
                                 .setIsProceduralPrimitive(true));
    }
    check(device->createRayTracingPipeline(pipeline, &m_pipeline), "create GVDB RT pipeline");
    for (int i = 0; i < 2; ++i)
    {
        auto &table = m_tables[i];
        check(m_pipeline->createShaderTable(rt::ShaderTableDesc(), &table), "create shader table");
        table->setRayGenerationShader("rayGeneration");
        if (table->addMissShader("radianceMiss") != 0 || table->addMissShader("shadowMiss") != 1 ||
            table->addHitGroup(i ? "surfaceRadiance" : "volumeRadiance") != 0 ||
            table->addHitGroup(i ? "surfaceShadow" : "volumeShadow") != 1)
            throw std::runtime_error("GVDB ray type table ordering failed");
    }
    const auto &info = volume.info();
    const rt::GeometryAABB bounds{info.boundsMin.x, info.boundsMin.y, info.boundsMin.z,
                                  info.boundsMax.x, info.boundsMax.y, info.boundsMax.z};
    check(device->createBuffer(BufferDesc()
                                   .setByteSize(sizeof(bounds))
                                   .setIsAccelStructBuildInput(true)
                                   .setInitialState(ResourceStates::CopyDest)
                                   .setKeepInitialState(true),
                               &m_bounds),
          "create volume bounds");
    runtime.upload(m_bounds, &bounds, sizeof(bounds));
    runtime.flush();
    rt::GeometryDesc geometry;
    geometry.setAABBs(rt::GeometryAABBs().setBuffer(m_bounds).setCount(1).setStride(sizeof(bounds)));
    check(device->createAccelStruct(rt::AccelStructDesc().addBottomLevelGeometry(geometry).setBuildFlags(
                                        rt::AccelStructBuildFlags::PreferFastTrace),
                                    &m_blas),
          "create volume BLAS");
    check(device->createAccelStruct(
              rt::AccelStructDesc().setTopLevelMaxInstances(1).setBuildFlags(rt::AccelStructBuildFlags::AllowUpdate),
              &m_tlas),
          "create volume TLAS");
    check(device->createCommandList(CommandListParameters(), &m_commands), "create RT commands");
    m_commands->open();
    m_commands->buildBottomLevelAccelStruct(m_blas, &geometry, 1);
    submit();
}
void SlangVolumeRT::submit()
{
    m_commands->close();
    if (!m_runtime->device()->executeCommandList(m_commands))
        throw std::runtime_error("GVDB RT submission failed");
}
RaycastOutput SlangVolumeRT::render(RaycastParameters p, const std::vector<Float4> &transfer, bool surface,
                                    const rt::AffineTransform &transform)
{
    validate(p, transfer, transform);
    p.volume = m_volume->info();
    p.image.z = uint32_t(transfer.size());
    auto device = m_runtime->device();
    auto transferBuffer = m_runtime->buffer(transfer.size() * sizeof(Float4), sizeof(Float4), transfer.data());
    RaycastOutput output;
    output.width = p.image.x;
    output.height = p.image.y;
    output.color = m_runtime->buffer(size_t(p.image.x) * p.image.y * sizeof(Float4), sizeof(Float4));
    output.depth = m_runtime->buffer(size_t(p.image.x) * p.image.y * sizeof(float), sizeof(float));
    BufferHandle parameters;
    check(device->createBuffer(BufferDesc()
                                   .setByteSize(256)
                                   .setIsConstantBuffer(true)
                                   .setInitialState(ResourceStates::ConstantBuffer)
                                   .setKeepInitialState(true),
                               &parameters),
          "create RT parameters");
    uint8_t padded[256]{};
    std::memcpy(padded, &p, sizeof(p));
    m_runtime->upload(parameters, padded, sizeof(padded));
    m_runtime->flush();
    const std::vector<BindingSetItem> available = {BindingSetItem::ConstantBuffer(0, parameters),
                                                   BindingSetItem::StructuredBuffer_SRV(0, m_volume->atlas()),
                                                   BindingSetItem::RawBuffer_SRV(1, m_volume->nodes()),
                                                   BindingSetItem::RawBuffer_SRV(2, m_volume->children()),
                                                   BindingSetItem::StructuredBuffer_SRV(3, m_volume->levels()),
                                                   BindingSetItem::StructuredBuffer_SRV(4, transferBuffer),
                                                   BindingSetItem::RayTracingAccelStruct(5, m_tlas),
                                                   BindingSetItem::StructuredBuffer_UAV(0, output.color),
                                                   BindingSetItem::StructuredBuffer_UAV(1, output.depth)};
    BindingSetDesc bindingDesc;
    for (const auto &reflected : m_layoutDesc.bindings)
    {
        auto found = std::find_if(available.begin(), available.end(), [&](const auto &item) {
            return item.type == reflected.type && item.slot == reflected.slot;
        });
        if (found == available.end())
            throw std::runtime_error("Unexpected GVDB RT reflected resource");
        bindingDesc.addItem(*found);
    }
    BindingSetHandle bindings;
    check(device->createBindingSet(bindingDesc, m_layout, &bindings), "create RT bindings");
    rt::InstanceDesc instance;
    instance.setBLAS(m_blas).setInstanceMask(255).setTransform(transform);
    m_commands->open();
    m_commands->buildTopLevelAccelStruct(
        m_tlas, &instance, 1, m_topBuilt ? rt::AccelStructBuildFlags::PerformUpdate : rt::AccelStructBuildFlags::None);
    m_commands->setRayTracingState(rt::State().setShaderTable(m_tables[surface ? 1 : 0]).addBindingSet(bindings));
    m_commands->dispatchRays(rt::DispatchRaysArguments().setDimensions(p.image.x, p.image.y));
    submit();
    m_topBuilt = true;
    return output;
}
} // namespace gvdb::slang

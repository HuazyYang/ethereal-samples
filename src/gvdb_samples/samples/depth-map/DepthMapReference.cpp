// SPDX-License-Identifier: Apache-2.0
// Test-only old structured-atlas path; production owns one public raw snapshot.
#include "DepthMapReference.h"
#include "DepthMapShaderData.h"
#include "ComputeRuntime.h"
#include <gvdb/GVDBSlangVolume.h>
#include <gvdb/slang/GVDBPortableVbx.h>
#include <donut/app/ApplicationBase.h>
#include <cstring>
#include <stdexcept>
namespace depth_map
{
void compareTypedReference(nvrhi::IDevice *device, const Scene &scene, const Images &image, const View &view)
{
    using namespace nvrhi;
    namespace g = gvdb::slang;
    const auto check = [](FRESULT result, const char *what) {
        if (NVRHI_FAILED(result))
            throw std::runtime_error(what);
    };
    const auto require = [](bool value, const char *what) {
        if (!value)
            throw std::runtime_error(what);
    };
    g::VbxRawVolume source;
    std::string error;
    require(g::loadRawVBX(scene.volumeVBX.data(), scene.volumeVBX.size(), source, error), error.c_str());
    require(source.channels.size() == 1 && source.channels[0].format == g::AtlasFormat::R32_FLOAT,
            "typed reference format");
    std::vector<float> atlas(source.channels[0].bytes.size() / 4);
    std::memcpy(atlas.data(), source.channels[0].bytes.data(), source.channels[0].bytes.size());
    const auto shaderRoot = donut::app::GetDirectoryWithExecutable() / "shaders";
    const auto format = donut::app::GetShaderTypeName(device->getGraphicsAPI());
    ethereal::slang::ComputeRuntime volumeRuntime(device, shaderRoot / "gvdb_slang" / format);
    g::SlangVolume volume(volumeRuntime, source.packed, atlas.data(), atlas.size());
    if (scene.refreshApron)
        volume.updateApron();
    auto transfer =
        volumeRuntime.buffer(scene.transfer.size() * sizeof(g::Float4), sizeof(g::Float4), scene.transfer.data());
    volumeRuntime.flush();
    TextureDesc desc;
    desc.width = image.width;
    desc.height = image.height;
    desc.format = Format::R32_FLOAT;
    desc.initialState = ResourceStates::CopyDest;
    desc.keepInitialState = true;
    TextureHandle depth, output;
    check(device->createTexture(desc, &depth), "typed depth texture");
    desc.format = Format::R32_UINT;
    desc.isUAV = true;
    desc.initialState = ResourceStates::UnorderedAccess;
    check(device->createTexture(desc, &output), "typed output texture");
    desc.isUAV = false;
    StagingTextureHandle staging;
    check(device->createStagingTexture(desc, CpuAccessMode::Read, &staging), "typed staging");
    CommandListHandle commands;
    check(device->createCommandList({}, &commands), "typed commands");
    commands->open();
    commands->writeTexture(depth, 0, 0, image.depth.data(), size_t(image.width) * 4);
    commands->close();
    device->executeCommandList(commands);
    ethereal::slang::ComputeRuntime runtime(device, shaderRoot / "depth_map" / format);
    const auto params = detail::clipParameters(volume.info(), image.width, image.height, view);
    runtime.dispatch(
        "DepthClipTyped", "clipVolume", &params, sizeof(params),
        {BindingSetItem::StructuredBuffer_SRV(0, volume.atlas()), BindingSetItem::RawBuffer_SRV(1, volume.nodes()),
         BindingSetItem::RawBuffer_SRV(2, volume.children()), BindingSetItem::StructuredBuffer_SRV(3, volume.levels()),
         BindingSetItem::StructuredBuffer_SRV(4, transfer), BindingSetItem::Texture_SRV(5, depth),
         BindingSetItem::Texture_UAV(0, output)},
        (image.width + 7) / 8, (image.height + 7) / 8);
    runtime.flush();
    commands->open();
    commands->copyTexture2(staging, {}, output, {});
    commands->close();
    device->executeCommandList(commands);
    require(device->waitForIdle(), "typed completion");
    size_t pitch = 0;
    const auto *bytes =
        static_cast<const uint8_t *>(device->mapStagingTexture(staging, {}, CpuAccessMode::Read, pitch));
    require(bytes && pitch >= size_t(image.width) * 4, "typed map");
    bool equal = true;
    for (uint32_t y = 0; y < image.height; ++y)
        equal &= std::memcmp(bytes + size_t(y) * pitch, image.volume.data() + size_t(y) * image.width,
                             size_t(image.width) * 4) == 0;
    device->unmapStagingTexture(staging);
    require(equal, "Owned raw depth clipping differs from old structured-atlas output");
}
} // namespace depth_map

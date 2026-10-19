// SPDX-License-Identifier: Apache-2.0
#include "SprayRender.h"
#include "ComputeRuntime.h"
#include <sample-utils/OwnedVolumeRawViews.h>
#include <stdexcept>
namespace gvdb::samples::spray {
using namespace slang;
std::vector<Float4> sprayTransfer(Shading mode) {
    std::vector<Float4> result(16384);
    const auto line = [&](float begin, float end, Float4 a, Float4 b) {
        const uint32_t lo = uint32_t(begin * 16384), hi = uint32_t(end * 16384);
        for (uint32_t i = lo; i < hi; ++i) {
            const float u = float(i - lo) / float(hi - lo);
            result[i] = {a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u,
                         a.w + (b.w - a.w) * u};
        }
    };
    if (mode == Shading::Section) {
        line(0, .2f, {0, 0, 0, 0}, {1, .5f, 0, .5f});
        line(.2f, .3f, {1, .5f, 0, .5f}, {0, 0, 0, 0});
        line(.3f, 1, {0, 0, 0, 0}, {0, 0, 0, 0});
    } else {
        line(0, .1f, {0, 0, 0, 0}, {0, 0, 0, 0});
        line(.1f, .25f, {0, 0, 0, 0}, {1, 1, 1, .8f});
        line(.25f, .5f, {1, 1, 1, .8f}, {0, 0, 0, 0});
        line(.5f, 1, {0, 0, 0, 0}, {0, 0, 0, 0});
    }
    return result;
}
RaycastOutput renderSpray(ethereal::slang::ComputeRuntime& runtime, SprayVolume& volume,
                          RaycastParameters camera, Shading mode, const std::vector<Float4>& transfer) {
    if (uint32_t(mode) > 4 || transfer.size() != 16384)
        throw std::invalid_argument("Spray shading or transfer table is invalid");
    sample::OwnedVolumeRawViews views(runtime.device(), volume.snapshot(), 0, 1);
    nvrhi::BufferHandle emptyChildren;
    if (!views.children.buffer) {
        const uint32_t missing[2] = {UINT32_MAX, UINT32_MAX};
        emptyChildren = runtime.buffer(8, 0, missing, true);
        views.children = {emptyChildren, 0, 8};
    }
    camera.volume = views.info.volume;
    camera.image.z = 16384;
    camera.image.w = 65536;
    validateRaycastParameters(camera, transfer);
    struct Parameters {
        RaycastParameters camera;
        UInt4 counts;
        Float4 sampling;
        UInt4 mode;
    };
    Parameters parameters{
        camera, {0,              256, 65536, 0},
         {.25f,           .3f, .01f,  0},
         {uint32_t(mode), 0,   0,     0}
    };
    const uint64_t pixels = uint64_t(camera.image.x) * camera.image.y;
    RaycastOutput output{runtime.buffer(size_t(pixels) * 16, 16), runtime.buffer(size_t(pixels) * 4, 4),
                         camera.image.x, camera.image.y};
    auto table = runtime.buffer(transfer.size() * 16, 16, transfer.data());
    using B = nvrhi::BindingSetItem;
    runtime.dispatch(
        "SprayRender", "renderSpray", &parameters, sizeof(parameters),
        {B::RawBuffer_SRV(0, views.scalar.raw.buffer, sample::OwnedVolumeRawViews::range(views.scalar.raw)),
         B::RawBuffer_SRV(1, views.nodes.buffer, sample::OwnedVolumeRawViews::range(views.nodes)),
         B::RawBuffer_SRV(2, views.children.buffer, sample::OwnedVolumeRawViews::range(views.children)),
         B::StructuredBuffer_SRV(3, views.levels.buffer, nvrhi::Format::UNKNOWN,
                                 sample::OwnedVolumeRawViews::range(views.levels)),
         B::StructuredBuffer_SRV(4, table),
         B::RawBuffer_SRV(5, views.color.raw.buffer, sample::OwnedVolumeRawViews::range(views.color.raw)),
         B::StructuredBuffer_UAV(0, output.color), B::StructuredBuffer_UAV(1, output.depth)},
        (camera.image.x + 7) / 8, (camera.image.y + 7) / 8);
    return output;
}
}  // namespace gvdb::samples::spray

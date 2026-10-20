// SPDX-License-Identifier: Apache-2.0
#include "SprayRender.h"
#include "ComputeRuntime.h"
#include <sample-utils/OwnedVolumeModes.h>
#include <sample-utils/OwnedVolumeSections.h>
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
    if (mode == Shading::Section) {
        sample::SectionOptions section;
        section.point = {50, 50, 50, 0};
        section.normal = {-1, 0, 0, 0};
        camera.direction00.w = 0;
        return sample::renderOwnedSection(runtime, volume.snapshot(), 0, 1, camera, transfer, section);
    }
    sample::VolumeModeOptions settings;
    settings.mode = static_cast<sample::VolumeMode>(mode);
    camera.direction00.w = 0;
    camera.march.x = .25f;
    camera.valueRange.z = .3f;
    return sample::renderOwnedMode(runtime, volume.snapshot(), 0, 1, camera, transfer, settings);
}
}  // namespace gvdb::samples::spray

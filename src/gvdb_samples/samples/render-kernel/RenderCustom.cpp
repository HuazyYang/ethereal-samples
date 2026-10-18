// SPDX-License-Identifier: Apache-2.0
#include "RenderCustom.h"
#include "ComputeRuntime.h"
#include <cmath>
#include <stdexcept>
gvdb::slang::RaycastOutput renderCustom(ethereal::slang::ComputeRuntime &runtime, gvdb::slang::SlangVolume &volume,
                                        gvdb::slang::RaycastParameters p)
{
    using namespace nvrhi;
    if (!p.image.x || !p.image.y || uint64_t(p.image.x) * p.image.y > UINT32_MAX || !p.image.w ||
        !std::isfinite(p.march.x) || p.march.x <= 0 || !std::isfinite(p.valueRange.z))
        throw std::invalid_argument("Invalid custom renderer image, step or density threshold");
    p.volume = volume.info();
    gvdb::slang::RaycastOutput output;
    output.width = p.image.x;
    output.height = p.image.y;
    output.color =
        runtime.buffer(size_t(p.image.x) * p.image.y * sizeof(gvdb::slang::Float4), sizeof(gvdb::slang::Float4));
    output.depth = runtime.buffer(size_t(p.image.x) * p.image.y * sizeof(float), sizeof(float));
    runtime.dispatch(
        "RenderCustom", "raycast_kernel", &p, sizeof(p),
        {BindingSetItem::StructuredBuffer_SRV(0, volume.atlas()), BindingSetItem::RawBuffer_SRV(1, volume.nodes()),
         BindingSetItem::RawBuffer_SRV(2, volume.children()), BindingSetItem::StructuredBuffer_SRV(3, volume.levels()),
         BindingSetItem::StructuredBuffer_UAV(0, output.color), BindingSetItem::StructuredBuffer_UAV(1, output.depth)},
        p.image.x / 8 + uint32_t(p.image.x % 8 != 0), p.image.y / 8 + uint32_t(p.image.y % 8 != 0));
    return output;
}

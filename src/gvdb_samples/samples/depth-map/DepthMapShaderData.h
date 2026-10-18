// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DepthMap.h"
#include <cmath>
namespace depth_map::detail
{
struct DepthParameters
{
    gvdb::slang::VolumeInfo volume;
    gvdb::slang::UInt4 image;
    gvdb::slang::Float4 origin, direction00, directionDx, directionDy;
    gvdb::slang::Float4 indexToWorldRows[3], cameraForward, nearFarStepBias, integration, background;
};
static_assert(sizeof(DepthParameters) == 272);
inline DepthParameters clipParameters(const gvdb::slang::VolumeInfo &volume, uint32_t w, uint32_t h, const View &view)
{
    const float horizontal = std::tan(dm::radians(25.f)), vertical = horizontal * float(h) / w;
    auto right = view.cameraToWorld.m_linear.row0, up = view.cameraToWorld.m_linear.row1,
         forward = view.cameraToWorld.m_linear.row2;
    const auto inverse = dm::inverse(view.indexToWorld);
    const auto f4 = [](dm::float3 v) -> gvdb::slang::Float4 { return {v.x, v.y, v.z, 0}; };
    DepthParameters p{};
    p.volume = volume;
    p.image = {w, h, 256, uint32_t(view.exactDepth)};
    p.origin = f4(inverse.transformPoint(view.cameraToWorld.m_translation));
    p.direction00 = f4(inverse.transformVector(forward - right * horizontal + up * vertical));
    p.directionDx = f4(inverse.transformVector(right * (2 * horizontal / w)));
    p.directionDy = f4(inverse.transformVector(up * (-2 * vertical / h)));
    p.indexToWorldRows[0] = f4(view.indexToWorld.m_linear.row0);
    p.indexToWorldRows[1] = f4(view.indexToWorld.m_linear.row1);
    p.indexToWorldRows[2] = f4(view.indexToWorld.m_linear.row2);
    p.cameraForward = f4(forward);
    p.nearFarStepBias = {view.nearPlane, view.farPlane, view.step, view.bias};
    p.integration = {1, 1, .005f, .005f};
    p.background = {.1f, .2f, .4f, 1};
    return p;
}
} // namespace depth_map::detail

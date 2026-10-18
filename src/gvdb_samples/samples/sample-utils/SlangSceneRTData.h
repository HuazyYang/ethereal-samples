// Explicit GPU layouts shared with SlangSceneRT.slang.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gvdb/slang/GVDBPortable.h>
namespace gvdb::sample::scene_gpu {
using slang::Float4;
using slang::UInt4;
struct alignas(16) Volume {
    slang::VolumeInfo info;
    Float4 steps, extinct, cutoff, threshold;
    UInt4 options;
};
struct alignas(16) Material {
    Float4 ambient, diffuse, specular, environment;
    Float4 reflection, refraction, optical, bias;
};
struct alignas(16) Parameters {
    slang::RaycastParameters camera;
    UInt4 state, random;
    Float4 pointLight;
};
static_assert(sizeof(Volume) == 160 && sizeof(Material) == 128 && sizeof(Parameters) == 272);
}  // namespace gvdb::sample::scene_gpu

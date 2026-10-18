// CPU-only OBJ and environment decoding shared by portable sample applications.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SlangSceneRT.h"
namespace gvdb::sample
{
// Retains the legacy area-weighted normal fallback and SNORM8 normal quantization.
SceneMesh loadSceneObj(const std::filesystem::path &path, float scale = 1, dm::float3 offset = dm::float3::zero());
SceneEnvironment loadSceneEnvironment(const std::filesystem::path &path);
} // namespace gvdb::sample

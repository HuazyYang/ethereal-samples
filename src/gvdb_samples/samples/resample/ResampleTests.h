// Independent CPU references for the migrated production resample pipeline.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ResampleVolume.h"
namespace gvdb::samples {
void verifyResampleModes(ResampleVolume& scene,const std::vector<float>& source,bool originalHeadAsset=false);
void runResampleSelfTests(ethereal::slang::ComputeRuntime& runtime);
} // namespace gvdb::samples

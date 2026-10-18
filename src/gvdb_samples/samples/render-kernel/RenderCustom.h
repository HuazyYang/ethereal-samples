// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../sample-utils/OwnedVolumeRaycast.h"
gvdb::slang::RaycastOutput renderCustom(ethereal::slang::ComputeRuntime& runtime,
    gvdb::portable::SnapshotHandle volume, gvdb::slang::RaycastParameters parameters);

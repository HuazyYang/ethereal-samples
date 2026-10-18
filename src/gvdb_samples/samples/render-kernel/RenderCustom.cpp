// SPDX-License-Identifier: Apache-2.0
#include "RenderCustom.h"
gvdb::slang::RaycastOutput renderCustom(ethereal::slang::ComputeRuntime& runtime,
    gvdb::portable::SnapshotHandle volume, gvdb::slang::RaycastParameters parameters) {
    // The retained custom kernel has this fixed miss color and no transfer lookup.
    // Match it for empty publications too; t4 is a valid unused module global.
    parameters.background={0,0,.1f,1};
    return gvdb::sample::dispatchOwnedRaycast(runtime,std::move(volume),0,parameters,{{0,0,0,0}},
        "RenderCustom","raycast_kernel");
}

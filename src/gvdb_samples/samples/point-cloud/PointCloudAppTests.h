// SPDX-License-Identifier: Apache-2.0
#pragma once
namespace ethereal::slang {
class ComputeRuntime;
}
namespace gvdb::samples::pointcloud {
void runPointCloudAppTests(ethereal::slang::ComputeRuntime& runtime);
void runPointCloudRayPolicyTests(ethereal::slang::ComputeRuntime& runtime);
}  // namespace gvdb::samples::pointcloud

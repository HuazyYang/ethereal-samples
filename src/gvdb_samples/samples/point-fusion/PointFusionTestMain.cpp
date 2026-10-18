// SPDX-License-Identifier: Apache-2.0
#include "PointFusionTests.h"
#include "ExampleDevice.h"
int main(int argc, const char** argv) {
    try {
        ethereal::slang::ExampleDevice device(argc, argv);
        ethereal::slang::ComputeRuntime runtime(device.get(), device.shaderPath());
        gvdb::samples::runPointFusionTests(runtime);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Point fusion tests: %s\n", error.what());
        return 1;
    }
}

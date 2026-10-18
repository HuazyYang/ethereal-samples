// Installed concrete operator factory link check, without a device.
#include "GVDBNvrhiVoxelOps.h"
#include <cstdio>
int main() {
    std::unique_ptr<gvdb::portable::NvrhiVoxelOps> ops;
    if(gvdb::portable::NvrhiVoxelOps::create({},ops)!=nvrhi::FE_INVALID_ARGS || ops)return 1;
    std::puts("Installed concrete voxel operator library links and validates input PASS");return 0;
}

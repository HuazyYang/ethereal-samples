#include "GVDBNvrhiVoxelFilters.h"
#include <cstdio>
int main() {
    std::unique_ptr<gvdb::portable::NvrhiVoxelFilters> filters;
    if(gvdb::portable::NvrhiVoxelFilters::create({},filters)!=nvrhi::FE_INVALID_ARGS || filters)return 1;
    std::puts("Installed filter core library links and validates factory input PASS");return 0;
}

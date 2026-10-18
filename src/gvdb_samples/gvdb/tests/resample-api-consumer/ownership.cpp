#include "GVDBNvrhiResample.h"
int main() {
    std::unique_ptr<gvdb::portable::NvrhiResample> resample;
    return gvdb::portable::NvrhiResample::create({},resample)==nvrhi::FE_INVALID_ARGS&&!resample ? 0 : 1;
}

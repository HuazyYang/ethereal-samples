#include "GVDBNvrhiDownsample.h"
int main(){
    using namespace gvdb::portable;
    std::unique_ptr<NvrhiDownsample> operation;
    return NvrhiDownsample::create({},operation)==nvrhi::FE_INVALID_ARGS&&!operation?0:1;
}

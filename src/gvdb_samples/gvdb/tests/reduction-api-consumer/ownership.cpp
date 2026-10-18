#include "GVDBNvrhiReduction.h"
int main() {
    std::unique_ptr<gvdb::portable::NvrhiReduction> reduction;
    std::unique_ptr<gvdb::portable::PreparedReduction> ticket;
    return gvdb::portable::NvrhiReduction::create({},reduction)==nvrhi::FE_INVALID_ARGS&&!reduction ? 0 : 1;
}

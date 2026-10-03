// gp-facing wrappers over the Thrust helpers in GPDeviceCUDAUtils.cu.
#include <gvdb/GPDeviceCUDAUtils.h>

namespace donut::gp {

namespace detail {
int thrustSortKeys64(void *cuContext, void *cuStream, uint64_t *keys, uint32_t count);
int thrustMinMaxFloat(void *cuContext, void *cuStream, float *values, uint32_t count, float *outMin, float *outMax);
}  // namespace detail

nvrhi::FRESULT sortKeys64(IDeviceQueue *queue, IBuffer *keys, uint64_t offset, uint32_t count) {
    if (!queue || !keys) return nvrhi::FE_INVALID_ARGS;
    if (count < 2) return nvrhi::FS_OK;
    if (offset + uint64_t(count) * sizeof(uint64_t) > keys->getDesc()->byteSize) return nvrhi::FE_INVALID_ARGS;
    auto *ptr = reinterpret_cast<uint64_t *>(keys->getNativeHandle() + offset);
    int err = detail::thrustSortKeys64((void *)queue->getDevice()->getNativeHandle(), (void *)queue->getNativeHandle(),
                                       ptr, count);
    return err == 0 ? nvrhi::FS_OK : nvrhi::FE_GENERIC_ERROR;
}

nvrhi::FRESULT minMaxFloat(IDeviceQueue *queue, IBuffer *values, uint64_t offset, uint32_t count,
                           float *outMin, float *outMax) {
    if (!queue || !values || count == 0) return nvrhi::FE_INVALID_ARGS;
    if (offset + uint64_t(count) * sizeof(float) > values->getDesc()->byteSize) return nvrhi::FE_INVALID_ARGS;
    auto *ptr = reinterpret_cast<float *>(values->getNativeHandle() + offset);
    int err = detail::thrustMinMaxFloat((void *)queue->getDevice()->getNativeHandle(), (void *)queue->getNativeHandle(),
                                        ptr, count, outMin, outMax);
    return err == 0 ? nvrhi::FS_OK : nvrhi::FE_GENERIC_ERROR;
}

}  // namespace donut::gp

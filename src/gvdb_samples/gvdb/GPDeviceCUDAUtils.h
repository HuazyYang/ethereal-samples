#ifndef GVDB_GPDEVICE_CUDA_UTILS_H
#define GVDB_GPDEVICE_CUDA_UTILS_H
// Device-wide primitives that are easier to get from the CUDA runtime
// libraries (Thrust/CUB) than from hand-written PTX. They work on gp
// buffers and run on the stream of the given gp queue.
#include <gvdb/GPDevice.h>

namespace donut::gp {

// Sorts `count` 64-bit keys in place (ascending), starting at `offset` bytes.
nvrhi::FRESULT sortKeys64(IDeviceQueue *queue, IBuffer *keys, uint64_t offset, uint32_t count);

// Writes min/max of `count` floats (at `offset` bytes) to outMin/outMax (host).
nvrhi::FRESULT minMaxFloat(IDeviceQueue *queue, IBuffer *values, uint64_t offset, uint32_t count,
                           float *outMin, float *outMax);

}  // namespace donut::gp

#endif /* GVDB_GPDEVICE_CUDA_UTILS_H */

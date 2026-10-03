// Thrust-backed helpers for the gp CUDA device. Compiled by nvcc as host
// code. This file deliberately includes no nvrhi / gp headers: nvcc's host
// pass does not accept the NVRHI_IID constexpr globals, so the gp-facing
// wrappers live in GPDeviceCUDAUtils.cpp and call the raw entry points below
// with native handles.
#include <cuda.h>
#include <cuda_runtime.h>
#include <thrust/device_ptr.h>
#include <thrust/sort.h>
#include <thrust/extrema.h>
#include <thrust/execution_policy.h>
#include <cstdint>

namespace donut::gp::detail {

namespace {
struct ContextScope {
    CUcontext ctx;
    explicit ContextScope(CUcontext c) : ctx(c) {
        if (ctx) cuCtxPushCurrent(ctx);
    }
    ~ContextScope() {
        if (ctx) {
            CUcontext dummy;
            cuCtxPopCurrent(&dummy);
        }
    }
};
}  // namespace

// Returns a cudaError_t value (0 on success).
int thrustSortKeys64(void *cuContext, void *cuStream, uint64_t *keys, uint32_t count) {
    ContextScope scope((CUcontext)cuContext);
    cudaStream_t stream = (cudaStream_t)cuStream;
    thrust::device_ptr<uint64_t> begin = thrust::device_pointer_cast(keys);
    thrust::sort(thrust::cuda::par.on(stream), begin, begin + count);
    return (int)cudaPeekAtLastError();
}

int thrustMinMaxFloat(void *cuContext, void *cuStream, float *values, uint32_t count, float *outMin, float *outMax) {
    ContextScope scope((CUcontext)cuContext);
    cudaStream_t stream = (cudaStream_t)cuStream;
    thrust::device_ptr<float> begin = thrust::device_pointer_cast(values);
    auto mm = thrust::minmax_element(thrust::cuda::par.on(stream), begin, begin + count);
    if (outMin) *outMin = *mm.first;
    if (outMax) *outMax = *mm.second;
    return (int)cudaPeekAtLastError();
}

}  // namespace donut::gp::detail

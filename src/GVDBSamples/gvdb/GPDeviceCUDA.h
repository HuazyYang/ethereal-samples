#ifndef SRC_GPDEVICE_GPDEVICE_CUDA_H
#define SRC_GPDEVICE_GPDEVICE_CUDA_H
#include "GPDevice.h"

namespace donut::gp {

struct CUDADeviceDesc {
    IMessageCallback *messageCallback;
};

FRESULT createCUDADevice(const CUDADeviceDesc &desc, IDevice **device);

}

#endif /* SRC_GPDEVICE_GPDEVICE_CUDA_H */

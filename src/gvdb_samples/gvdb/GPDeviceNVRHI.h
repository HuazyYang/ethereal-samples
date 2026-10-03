#ifndef GPDEVICENVRHI_H
#define GPDEVICENVRHI_H
#include <nvrhi/nvrhi.h>
#include <gvdb/GPDevice.h>

namespace donut {

NVRHI_IID(IGPAndNVRHIInteropDevice, "fa31dc7a-00b5-41d4-bf29-d82b771248ae")
struct IGPAndNVRHIInteropDevice : public nvrhi::IObject {
    NVRHI_DECLARE_UUID_TRAITS(IGPAndNVRHIInteropDevice)
    virtual nvrhi::IDevice *getNVRHIDevice() = 0;
    virtual gp::IDevice *getGPDevice() = 0;

    virtual nvrhi::FRESULT createGPBuffer(nvrhi::IBuffer *srcBuffer, gp::IBuffer **dstBuffer) = 0;
    virtual nvrhi::FRESULT createGPTexture(nvrhi::ITexture *srcTexture, gp::ITexture **dstTexture) = 0;

    virtual nvrhi::FRESULT acquireNVRHIBufferKeyedMutex(nvrhi::IBuffer *buffer, uint32_t key,
                                                 uint32_t timeoutMS) = 0;
    virtual nvrhi::FRESULT releaseNVRHIBufferKeyedMutex(nvrhi::IBuffer *buffer, uint32_t key) = 0;

    virtual nvrhi::FRESULT acquireNVRHITextureKeyedMutex(nvrhi::ITexture *texture, uint32_t key,
                                                  uint32_t timeoutMS) = 0;
    virtual nvrhi::FRESULT releaseNVRHITextureKeyedMutex(nvrhi::ITexture *texture,
                                                  uint32_t key) = 0;

    virtual nvrhi::FRESULT acquireGPResourceKeyedMutexes(
        gp::IDeviceQueue *gpQueue,
        const gp::GraphicsInteropKeyedMutexWaitParams *paramsArray,
        uint32_t numParamsArray) = 0;
    virtual nvrhi::FRESULT releaseGPResourceKeyedMutexes(
        gp::IDeviceQueue *gpQueue,
        const gp::GraphicsInteropKeyedMutexSignalParams *paramsArray,
        uint32_t numParamsArray) = 0;

    virtual nvrhi::FRESULT commitGPQueueSignal(gp::IDeviceQueue *queue, nvrhi::CommandQueue queueType) = 0;

    virtual nvrhi::FRESULT commitGPQueueWait(gp::IDeviceQueue *queue, nvrhi::CommandQueue queueType) = 0;

    virtual nvrhi::FRESULT commitNVRHIQueueSignal(nvrhi::CommandQueue queueType) = 0;

    virtual nvrhi::FRESULT commitNVRHIQueueWait(nvrhi::CommandQueue queueType) = 0;
};

nvrhi::FRESULT createGPAndNVRHIDevice(nvrhi::IDevice *nvrhiDevice, gp::IDevice *gpDevice,
                               IGPAndNVRHIInteropDevice **interopDevice);

};  // namespace donut::gp

#endif /* GPDEVICENVRHI_H */

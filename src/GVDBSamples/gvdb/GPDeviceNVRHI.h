#ifndef GPDEVICENVRHI_H
#define GPDEVICENVRHI_H
#include <nvrhi/nvrhi.h>
#include <gvdb/GPDevice.h>

namespace donut {

DONUT_IID(IGPAndNVRHIInteropDevice, "fa31dc7a-00b5-41d4-bf29-d82b771248ae")
struct IGPAndNVRHIInteropDevice : public IObject {
    DONUT_DECLARE_UUID_TRAITS(IGPAndNVRHIInteropDevice)
    virtual nvrhi::IDevice *getNVRHIDevice() = 0;
    virtual gp::IDevice *getGPDevice() = 0;

    virtual FRESULT createGPBuffer(nvrhi::IBuffer *srcBuffer, gp::IBuffer **dstBuffer) = 0;
    virtual FRESULT createGPTexture(nvrhi::ITexture *srcTexture, gp::ITexture **dstTexture) = 0;

    virtual FRESULT acquireNVRHIBufferKeyedMutex(nvrhi::IBuffer *buffer, uint32_t key,
                                                 uint32_t timeoutMS) = 0;
    virtual FRESULT releaseNVRHIBufferKeyedMutex(nvrhi::IBuffer *buffer, uint32_t key) = 0;

    virtual FRESULT acquireNVRHITextureKeyedMutex(nvrhi::ITexture *texture, uint32_t key,
                                                  uint32_t timeoutMS) = 0;
    virtual FRESULT releaseNVRHITextureKeyedMutex(nvrhi::ITexture *texture,
                                                  uint32_t key) = 0;

    virtual FRESULT acquireGPResourceKeyedMutexes(
        gp::IDeviceQueue *gpQueue,
        const gp::GraphicsInteropKeyedMutexWaitParams *paramsArray,
        uint32_t numParamsArray) = 0;
    virtual FRESULT releaseGPResourceKeyedMutexes(
        gp::IDeviceQueue *gpQueue,
        const gp::GraphicsInteropKeyedMutexSignalParams *paramsArray,
        uint32_t numParamsArray) = 0;

    virtual FRESULT commitGPQueueSignal(gp::IDeviceQueue *queue, nvrhi::CommandQueue queueType) = 0;

    virtual FRESULT commitGPQueueWait(gp::IDeviceQueue *queue, nvrhi::CommandQueue queueType) = 0;

    virtual FRESULT commitNVRHIQueueSignal(nvrhi::CommandQueue queueType) = 0;

    virtual FRESULT commitNVRHIQueueWait(nvrhi::CommandQueue queueType) = 0;
};

FRESULT createGPAndNVRHIDevice(nvrhi::IDevice *nvrhiDevice, gp::IDevice *gpDevice,
                               IGPAndNVRHIInteropDevice **interopDevice);

};  // namespace donut::gp

#endif /* GPDEVICENVRHI_H */

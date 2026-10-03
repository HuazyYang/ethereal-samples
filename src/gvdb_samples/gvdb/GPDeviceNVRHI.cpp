#include <gvdb/GPDeviceNVRHI.h>
#include <nvrhi/core/foundation.h>
#include <nvrhi/core/autoptr.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

namespace donut {

gp::Format getFormatFromNVRHIForamt(nvrhi::Format nvrhiFormat) {
    using namespace gp;
    // clang-format off
    switch (nvrhiFormat) {
    case nvrhi::Format::UNKNOWN          : return Format::UNKNOWN          ;
    case nvrhi::Format::R8_UINT          : return Format::R8_UINT          ;
    case nvrhi::Format::R8_SINT          : return Format::R8_SINT          ;
    case nvrhi::Format::R8_UNORM         : return Format::R8_UNORM         ;
    case nvrhi::Format::R8_SNORM         : return Format::R8_SNORM         ;
    case nvrhi::Format::RG8_UINT         : return Format::RG8_UINT         ;
    case nvrhi::Format::RG8_SINT         : return Format::RG8_SINT         ;
    case nvrhi::Format::RG8_UNORM        : return Format::RG8_UNORM        ;
    case nvrhi::Format::RG8_SNORM        : return Format::RG8_SNORM        ;
    case nvrhi::Format::RGBA8_UINT       : return Format::RGBA8_UINT       ;
    case nvrhi::Format::RGBA8_SINT       : return Format::RGBA8_SINT       ;
    case nvrhi::Format::RGBA8_UNORM      : return Format::RGBA8_UNORM      ;
    case nvrhi::Format::RGBA8_SNORM      : return Format::RGBA8_SNORM      ;
    case nvrhi::Format::BGRA8_UNORM      : return Format::BGRA8_UNORM      ;
    case nvrhi::Format::R16_UINT         : return Format::R16_UINT         ;
    case nvrhi::Format::R16_SINT         : return Format::R16_SINT         ;
    case nvrhi::Format::R16_FLOAT        : return Format::R16_FLOAT        ;
    case nvrhi::Format::R16_UNORM        : return Format::R16_UNORM        ;
    case nvrhi::Format::R16_SNORM        : return Format::R16_SNORM        ;
    case nvrhi::Format::RG16_UINT        : return Format::RG16_UINT        ;
    case nvrhi::Format::RG16_SINT        : return Format::RG16_SINT        ;
    case nvrhi::Format::RG16_FLOAT       : return Format::RG16_FLOAT       ;
    case nvrhi::Format::RG16_UNORM       : return Format::RG16_UNORM       ;
    case nvrhi::Format::RG16_SNORM       : return Format::RG16_SNORM       ;
    case nvrhi::Format::RGBA16_UINT      : return Format::RGBA16_UINT      ;
    case nvrhi::Format::RGBA16_SINT      : return Format::RGBA16_SINT      ;
    case nvrhi::Format::RGBA16_FLOAT     : return Format::RGBA16_FLOAT     ;
    case nvrhi::Format::RGBA16_UNORM     : return Format::RGBA16_UNORM     ;
    case nvrhi::Format::RGBA16_SNORM     : return Format::RGBA16_SNORM     ;
    case nvrhi::Format::R32_UINT         : return Format::R32_UINT         ;
    case nvrhi::Format::R32_SINT         : return Format::R32_SINT         ;
    case nvrhi::Format::R32_FLOAT        : return Format::R32_FLOAT        ;
    case nvrhi::Format::RG32_UINT        : return Format::RG32_UINT        ;
    case nvrhi::Format::RG32_SINT        : return Format::RG32_SINT        ;
    case nvrhi::Format::RG32_FLOAT       : return Format::RG32_FLOAT       ;
    case nvrhi::Format::RGB32_UINT       : return Format::RGB32_UINT       ;
    case nvrhi::Format::RGB32_SINT       : return Format::RGB32_SINT       ;
    case nvrhi::Format::RGB32_FLOAT      : return Format::RGB32_FLOAT      ;
    case nvrhi::Format::RGBA32_UINT      : return Format::RGBA32_UINT      ;
    case nvrhi::Format::RGBA32_SINT      : return Format::RGBA32_SINT      ;
    case nvrhi::Format::RGBA32_FLOAT     : return Format::RGBA32_FLOAT     ;
    case nvrhi::Format::R10G10B10A2_UNORM: return Format::R10G10B10A2_UNORM;
    case nvrhi::Format::BC1_UNORM        : return Format::BC1_UNORM        ;
    case nvrhi::Format::BC1_UNORM_SRGB   : return Format::BC1_UNORM_SRGB   ;
    case nvrhi::Format::BC2_UNORM        : return Format::BC2_UNORM        ;
    case nvrhi::Format::BC2_UNORM_SRGB   : return Format::BC2_UNORM_SRGB   ;
    case nvrhi::Format::BC3_UNORM        : return Format::BC3_UNORM        ;
    case nvrhi::Format::BC3_UNORM_SRGB   : return Format::BC3_UNORM_SRGB   ;
    case nvrhi::Format::BC4_UNORM        : return Format::BC4_UNORM        ;
    case nvrhi::Format::BC4_SNORM        : return Format::BC4_SNORM        ;
    case nvrhi::Format::BC5_UNORM        : return Format::BC5_UNORM        ;
    case nvrhi::Format::BC5_SNORM        : return Format::BC5_SNORM        ;
    case nvrhi::Format::BC6H_UFLOAT      : return Format::BC6H_UFLOAT      ;
    case nvrhi::Format::BC6H_SFLOAT      : return Format::BC6H_SFLOAT      ;
    case nvrhi::Format::BC7_UNORM        : return Format::BC7_UNORM        ;
    case nvrhi::Format::BC7_UNORM_SRGB   : return Format::BC7_UNORM_SRGB   ;
    }
    // clang-format on
    return Format::UNKNOWN;
}

union NVRHITimelineSemaphore {
    ID3D11Fence *d3d11Fence;
    ID3D12Fence* d3d12Fence;
    VkSemaphore vkSemaphore;
};

struct TimelineTransaction {
    NVRHITimelineSemaphore nvrhiSemaphore;
    gp::IGraphicsInteropSemaphore* gpSemaphore;
    std::atomic<uint64_t> signalValue;
    uint64_t waitValue;
};

struct GPAndNVRHIInteropDevice : public nvrhi::ObjectImpl<IGPAndNVRHIInteropDevice> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GPAndNVRHIInteropDevice)
    NVRHI_IMPLEMENTS_INTERFACE(IGPAndNVRHIInteropDevice)
    NVRHI_END_INTERFACE_TABLE()

    nvrhi::IDevice* getNVRHIDevice() override;
    gp::IDevice* getGPDevice() override;

    nvrhi::FRESULT createGPBuffer(nvrhi::IBuffer* srcBuffer, gp::IBuffer** dstBuffer) override;
    nvrhi::FRESULT createGPTexture(nvrhi::ITexture* srcTexture,
                            gp::ITexture** dstTexture) override;

     nvrhi::FRESULT acquireNVRHIBufferKeyedMutex(nvrhi::IBuffer* buffer, uint32_t key,
                                                 uint32_t timeoutMS) override;
     nvrhi::FRESULT releaseNVRHIBufferKeyedMutex(nvrhi::IBuffer* buffer, uint32_t key) override;

     nvrhi::FRESULT acquireNVRHITextureKeyedMutex(nvrhi::ITexture* texture, uint32_t key,
                                                  uint32_t timeoutMS) override;
     nvrhi::FRESULT releaseNVRHITextureKeyedMutex(nvrhi::ITexture* texture,
                                                  uint32_t key) override;

     nvrhi::FRESULT acquireGPResourceKeyedMutexes(
         gp::IDeviceQueue *gpQueue,
        const gp::GraphicsInteropKeyedMutexWaitParams* paramsArray,
        uint32_t numParamsArray) override;
     nvrhi::FRESULT releaseGPResourceKeyedMutexes(
         gp::IDeviceQueue* gpQueue,
         const gp::GraphicsInteropKeyedMutexSignalParams* paramsArray,
         uint32_t numParamsArray) override;

      nvrhi::FRESULT commitGPQueueSignal(gp::IDeviceQueue* queue, nvrhi::CommandQueue queueType) override;

      nvrhi::FRESULT commitGPQueueWait(gp::IDeviceQueue* queue, nvrhi::CommandQueue queueType) override;

      nvrhi::FRESULT commitNVRHIQueueSignal(nvrhi::CommandQueue queueType) override;

      nvrhi::FRESULT commitNVRHIQueueWait(nvrhi::CommandQueue queueType) override;

     // Implements
     GPAndNVRHIInteropDevice(nvrhi::IDevice* nvrhiDevice, gp::IDevice* gpDevice);
     ~GPAndNVRHIInteropDevice();

     nvrhi::FRESULT init();

     nvrhi::IDevice* m_nvrhiDevice;
     gp::IDevice* m_gpDevice;
     TimelineTransaction m_transactions[3];
};

nvrhi::IDevice* GPAndNVRHIInteropDevice::getNVRHIDevice() {
    return m_nvrhiDevice;
}

gp::IDevice* GPAndNVRHIInteropDevice::getGPDevice() { return m_gpDevice; }

nvrhi::FRESULT GPAndNVRHIInteropDevice::createGPBuffer(nvrhi::IBuffer* nvrhiBuffer,
                                                gp::IBuffer** buffer) {
    nvrhi::GraphicsAPI rhiAPI = m_nvrhiDevice->getGraphicsAPI();
    switch (rhiAPI) {
        case nvrhi::GraphicsAPI::D3D11: {
            auto d3d11Buffer =
                static_cast<ID3D11Resource*>(nvrhiBuffer->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource));
            return m_gpDevice->createInteropD3D11Buffer(d3d11Buffer, 0, 0, buffer);
        }
        case nvrhi::GraphicsAPI::D3D12: {
            auto d3d12Resource =
                static_cast<ID3D12Resource*>(nvrhiBuffer->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource));
            return m_gpDevice->createInteropD3D12Buffer(d3d12Resource, 0, 0, buffer);
        }
        case nvrhi::GraphicsAPI::VULKAN: {
            auto vkBuffer = nvrhiBuffer->getNativeObject(nvrhi::ObjectTypes::VK_Buffer);
            auto vkMemory =
                nvrhiBuffer->getNativeObject(nvrhi::ObjectTypes::VK_DeviceMemory);
            auto vkDevice = m_nvrhiDevice->getNativeObject(nvrhi::ObjectTypes::VK_Device);
            return m_gpDevice->createInteropVulkanBuffer(vkBuffer, vkMemory, vkDevice, 0, 0,
                                                     buffer);
        }
    }
    return nvrhi::FS_OK;
}

nvrhi::FRESULT GPAndNVRHIInteropDevice::createGPTexture(nvrhi::ITexture* nvrhiTexture, gp::ITexture** texture) {
    nvrhi::GraphicsAPI rhiAPI = m_nvrhiDevice->getGraphicsAPI();

    switch (rhiAPI) {
        case nvrhi::GraphicsAPI::D3D11: {
            auto d3d11Resource =
                static_cast<ID3D11Resource*>(nvrhiTexture->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource));
            return m_gpDevice->createInteropD3D11Texture(d3d11Resource, texture);
        }
        case nvrhi::GraphicsAPI::D3D12: {
            auto d3d12Resource =
                static_cast<ID3D12Resource*>(nvrhiTexture->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource));
            return m_gpDevice->createInteropD3D12Texture(d3d12Resource, texture);
        }
        case nvrhi::GraphicsAPI::VULKAN: {
            auto vkImage = nvrhiTexture->getNativeObject(nvrhi::ObjectTypes::VK_Image);
            auto vkMemory =
                nvrhiTexture->getNativeObject(nvrhi::ObjectTypes::VK_DeviceMemory);
            auto vkDevice = m_nvrhiDevice->getNativeObject(nvrhi::ObjectTypes::VK_Device);

            auto& nvrhiDesc = nvrhiTexture->getDesc();
            gp::TextureDesc texDesc;
            switch (nvrhiDesc.dimension) {
            case nvrhi::TextureDimension::Texture1D:
            case nvrhi::TextureDimension::Texture1DArray:
                texDesc.dimension = gp::TextureDimension::Texture1D;
                texDesc.width = nvrhiDesc.width;
                texDesc.depthOrArraySize =
                    nvrhiDesc.dimension == nvrhi::TextureDimension::Texture1D
                        ? 1
                        : nvrhiDesc.arraySize;
                break;
            case nvrhi::TextureDimension::Texture2D:
            case nvrhi::TextureDimension::Texture2DArray:
                texDesc.dimension = gp::TextureDimension::Texture2D;
                texDesc.width = nvrhiDesc.width;
                texDesc.height = nvrhiDesc.height;
                texDesc.depthOrArraySize =
                    nvrhiDesc.dimension == nvrhi::TextureDimension::Texture2D
                        ? 1
                        : nvrhiDesc.arraySize;
                break;
            case nvrhi::TextureDimension::Texture3D:
                texDesc.dimension = gp::TextureDimension::Texture3D;
                texDesc.width = nvrhiDesc.width;
                texDesc.height = nvrhiDesc.height;
                texDesc.depthOrArraySize = nvrhiDesc.depth;
                break;
            default:
                return nvrhi::FE_INVALID_ARGS;
            }

            texDesc.mipLevels = nvrhiDesc.mipLevels;
            texDesc.format = getFormatFromNVRHIForamt(nvrhiDesc.format);

            if (texDesc.format == gp::Format::UNKNOWN) return nvrhi::FE_INVALID_ARGS;

            return m_gpDevice->createInteropVulkanTexture(vkImage, vkMemory, vkDevice, texDesc, texture);
        }
    }

    return nvrhi::FS_OK;
}

nvrhi::FRESULT GPAndNVRHIInteropDevice::acquireNVRHIBufferKeyedMutex(nvrhi::IBuffer* _buffer,
                                                              uint32_t key,
                                                              uint32_t timeoutMS) {
    if (!_buffer) return nvrhi::FE_INVALID_ARGS;

    if (m_nvrhiDevice->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11) return nvrhi::FS_OK;

    HRESULT hr;
    ID3D11Resource* resource = static_cast<ID3D11Resource*>(_buffer->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource));
    nvrhi::AutoPtr<IDXGIKeyedMutex> dxgiKeyedMutex;
   if(FAILED(hr = resource->QueryInterface(IID_PPV_ARGS(&dxgiKeyedMutex)))) {
       return nvrhi::FE_GENERIC_ERROR;
   }

   if (FAILED(hr = dxgiKeyedMutex->AcquireSync(key, timeoutMS))) return nvrhi::FE_GENERIC_ERROR;

   return nvrhi::FS_OK;
}

nvrhi::FRESULT GPAndNVRHIInteropDevice::releaseNVRHIBufferKeyedMutex(nvrhi::IBuffer* _buffer,
                                                              uint32_t key) {
    if (!_buffer) return nvrhi::FE_INVALID_ARGS;

    if (m_nvrhiDevice->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11) return nvrhi::FS_OK;

    HRESULT hr;
    ID3D11Resource* resource = static_cast<ID3D11Resource*>(_buffer->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource));
    nvrhi::AutoPtr<IDXGIKeyedMutex> dxgiKeyedMutex;
    if (FAILED(hr = resource->QueryInterface(IID_PPV_ARGS(&dxgiKeyedMutex)))) {
        return nvrhi::FE_GENERIC_ERROR;
    }

    if (FAILED(hr = dxgiKeyedMutex->ReleaseSync(key))) return nvrhi::FE_GENERIC_ERROR;

    return nvrhi::FS_OK;
}

nvrhi::FRESULT GPAndNVRHIInteropDevice::acquireNVRHITextureKeyedMutex(nvrhi::ITexture* _texture,
                                                               uint32_t key,
                                                               uint32_t timeoutMS) {
    if (!_texture) return nvrhi::FE_INVALID_ARGS;

    if (m_nvrhiDevice->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11) return nvrhi::FS_OK;

    HRESULT hr;
    ID3D11Resource* resource = static_cast<ID3D11Resource*>(_texture->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource));
    nvrhi::AutoPtr<IDXGIKeyedMutex> dxgiKeyedMutex;
    if (FAILED(hr = resource->QueryInterface(IID_PPV_ARGS(&dxgiKeyedMutex)))) {
        return nvrhi::FE_GENERIC_ERROR;
    }

    if (FAILED(hr = dxgiKeyedMutex->AcquireSync(key, timeoutMS))) return nvrhi::FE_GENERIC_ERROR;

    return nvrhi::FS_OK;
}

nvrhi::FRESULT GPAndNVRHIInteropDevice::releaseNVRHITextureKeyedMutex(nvrhi::ITexture* _texture,
                                                               uint32_t key) {
    if (!_texture) return nvrhi::FE_INVALID_ARGS;

    if (m_nvrhiDevice->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11) return nvrhi::FS_OK;

    HRESULT hr;
    ID3D11Resource* resource = static_cast<ID3D11Resource*>(_texture->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource));
    nvrhi::AutoPtr<IDXGIKeyedMutex> dxgiKeyedMutex;
    if (FAILED(hr = resource->QueryInterface(IID_PPV_ARGS(&dxgiKeyedMutex)))) {
        return nvrhi::FE_GENERIC_ERROR;
    }

    if (FAILED(hr = dxgiKeyedMutex->ReleaseSync(key))) return nvrhi::FE_GENERIC_ERROR;

    return nvrhi::FS_OK;
}

nvrhi::FRESULT GPAndNVRHIInteropDevice::acquireGPResourceKeyedMutexes(
    gp::IDeviceQueue* gpQueue, const gp::GraphicsInteropKeyedMutexWaitParams* paramsArray,
    uint32_t numParamsArray) {
    if (!gpQueue) return nvrhi::FE_INVALID_ARGS;

    if (m_nvrhiDevice->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11) return nvrhi::FS_OK;

    return gpQueue->acquireInteropKeyedMutexes(paramsArray, numParamsArray);
}

nvrhi::FRESULT GPAndNVRHIInteropDevice::releaseGPResourceKeyedMutexes(
    gp::IDeviceQueue* gpQueue, const gp::GraphicsInteropKeyedMutexSignalParams* paramsArray,
    uint32_t numParamsArray) {
    if (!gpQueue) return nvrhi::FE_INVALID_ARGS;

    if (m_nvrhiDevice->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11) return nvrhi::FS_OK;

    return gpQueue->releaseInteropKeyedMutexes(paramsArray, numParamsArray);
}

nvrhi::FRESULT GPAndNVRHIInteropDevice::commitGPQueueSignal(gp::IDeviceQueue* queue,
                                                     nvrhi::CommandQueue queueType) {

    if (!queue)
        return nvrhi::FE_INVALID_ARGS;

    int queueIndex = (int)queueType;
    if (m_nvrhiDevice->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11) queueIndex = 0;

    auto& transaction = m_transactions[queueIndex];
    uint64_t signalValue = transaction.signalValue.fetch_add(1, std::memory_order_relaxed);
    nvrhi::FRESULT fr;
    if (NVRHI_FAILED(fr = queue->signalInteropSemaphore(transaction.gpSemaphore, signalValue))) {
        NVRHI_ASSERT(0);
        return fr;
    }
    transaction.waitValue = signalValue;
    return nvrhi::FS_OK;
}

nvrhi::FRESULT GPAndNVRHIInteropDevice::commitGPQueueWait(gp::IDeviceQueue* queue,
                                                   nvrhi::CommandQueue queueType) {
    if (!queue) return nvrhi::FE_INVALID_ARGS;

    int queueIndex = (int)queueType;
    if (m_nvrhiDevice->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11) queueIndex = 0;

    auto& transaction = m_transactions[queueIndex];
    uint64_t waitValue = transaction.waitValue;
    nvrhi::FRESULT fr;
    if (NVRHI_FAILED(fr = queue->waitInteropSemaphoreAsync(transaction.gpSemaphore, waitValue))) {
        NVRHI_ASSERT(0);
        return fr;
    }
    return nvrhi::FS_OK;
}

nvrhi::FRESULT GPAndNVRHIInteropDevice::commitNVRHIQueueSignal(nvrhi::CommandQueue queueType) {
    switch (m_nvrhiDevice->getGraphicsAPI()) {
        case nvrhi::GraphicsAPI::D3D11: {
            auto& transaction = m_transactions[0];
            uint64_t signalValue = transaction.signalValue.fetch_add(1, std::memory_order_relaxed);

            HRESULT hr;
            auto d3d11Context = (ID3D11DeviceContext*)m_nvrhiDevice->getNativeObject(
                nvrhi::ObjectTypes::D3D11_DeviceContext);
            nvrhi::AutoPtr<ID3D11DeviceContext4> d3d11Context4;
            if (FAILED(hr = d3d11Context->QueryInterface(IID_PPV_ARGS(&d3d11Context4)))) {
                NVRHI_ASSERT(0);
                return nvrhi::FE_GENERIC_ERROR;
            }

            if (FAILED(hr = d3d11Context4->Signal(transaction.nvrhiSemaphore.d3d11Fence,
                                                signalValue))) {
                NVRHI_ASSERT(0);
                return nvrhi::FE_GENERIC_ERROR;
            }
            transaction.waitValue = signalValue;
        } break;
        case nvrhi::GraphicsAPI::D3D12: {
            auto& transaction = m_transactions[(int)queueType];
            uint64_t signalValue = transaction.signalValue.fetch_add(1, std::memory_order_relaxed);

            HRESULT hr;
            auto d3d12Queue = (ID3D12CommandQueue*)m_nvrhiDevice->getNativeQueue(
                nvrhi::ObjectTypes::D3D12_CommandQueue, queueType);

            if (FAILED(hr = d3d12Queue->Signal(transaction.nvrhiSemaphore.d3d12Fence,
                                             signalValue))) {
                NVRHI_ASSERT(0);
                return nvrhi::FE_GENERIC_ERROR;
            }
            transaction.waitValue = signalValue;
        } break;
        case nvrhi::GraphicsAPI::VULKAN: {
            auto& transaction = m_transactions[(int)queueType];
            uint64_t signalValue = transaction.signalValue.fetch_add(1, std::memory_order_relaxed);
            uint64_t waitValue = transaction.waitValue;

            vk::Result vkRet;
            auto vkQueue = vk::Queue((VkQueue)m_nvrhiDevice->getNativeQueue(
                nvrhi::ObjectTypes::VK_Queue, queueType));

            vk::TimelineSemaphoreSubmitInfo timelineInfo;

            timelineInfo.setWaitSemaphoreValueCount(1).setPWaitSemaphoreValues(&waitValue);
            timelineInfo.setSignalSemaphoreValueCount(1).setPSignalSemaphoreValues(
                &signalValue);

            vk::SubmitInfo submitInfo;
            vk::Semaphore timelineSemaphore{transaction.nvrhiSemaphore.vkSemaphore};
            vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eAllCommands;
            submitInfo.setPNext(&timelineInfo)
                .setWaitSemaphoreCount(1)
                .setPWaitSemaphores(&timelineSemaphore)
                .setPWaitDstStageMask(&waitStage)
                .setSignalSemaphoreCount(1)
                .setPSignalSemaphores(&timelineSemaphore);

            vkRet = vkQueue.submit(1, &submitInfo, vk::Fence{});
            if (vkRet != vk::Result::eSuccess) {
                NVRHI_ASSERT(0);
                return nvrhi::FE_GENERIC_ERROR;
            }
            transaction.waitValue = signalValue;
        } break;
    }

    return nvrhi::FS_OK;
}

nvrhi::FRESULT GPAndNVRHIInteropDevice::commitNVRHIQueueWait(nvrhi::CommandQueue queueType) {
    switch (m_nvrhiDevice->getGraphicsAPI()) {
        case nvrhi::GraphicsAPI::D3D11: {
            auto& transaction = m_transactions[0];
            uint64_t waitValue = transaction.waitValue;

            HRESULT hr;
            auto d3d11Context = (ID3D11DeviceContext*)m_nvrhiDevice->getNativeObject(
                nvrhi::ObjectTypes::D3D11_DeviceContext);
            nvrhi::AutoPtr<ID3D11DeviceContext4> d3d11Context4;
            if (FAILED(hr = d3d11Context->QueryInterface(IID_PPV_ARGS(&d3d11Context4)))) {
                NVRHI_ASSERT(0);
                return nvrhi::FE_GENERIC_ERROR;
            }

            if (FAILED(hr = d3d11Context4->Wait(transaction.nvrhiSemaphore.d3d11Fence,
                                                waitValue))) {
                NVRHI_ASSERT(0);
                return nvrhi::FE_GENERIC_ERROR;
            }
        } break;
        case nvrhi::GraphicsAPI::D3D12: {
            auto& transaction = m_transactions[(int)queueType];
            uint64_t waitValue = transaction.waitValue;

            HRESULT hr;
            auto d3d12Queue = (ID3D12CommandQueue*)m_nvrhiDevice->getNativeQueue(
                nvrhi::ObjectTypes::D3D12_CommandQueue, queueType);

            if (FAILED(hr = d3d12Queue->Wait(transaction.nvrhiSemaphore.d3d12Fence,
                                             waitValue))) {
                NVRHI_ASSERT(0);
                return nvrhi::FE_GENERIC_ERROR;
            }
        } break;
        case nvrhi::GraphicsAPI::VULKAN: {
            auto& transaction = m_transactions[(int)queueType];
            uint64_t waitValue = transaction.waitValue;
            uint64_t signalValue = transaction.signalValue.fetch_add(1, std::memory_order_relaxed);

            vk::Result vkRet;
            auto vkQueue = vk::Queue((VkQueue)m_nvrhiDevice->getNativeQueue(
                nvrhi::ObjectTypes::VK_Queue, queueType));

            vk::TimelineSemaphoreSubmitInfo timelineInfo;

            timelineInfo.setWaitSemaphoreValueCount(1).setPWaitSemaphoreValues(
                &waitValue);
            timelineInfo.setSignalSemaphoreValueCount(1).setPSignalSemaphoreValues(
                &signalValue);

            vk::SubmitInfo submitInfo;
            vk::Semaphore timelineSemaphore{transaction.nvrhiSemaphore.vkSemaphore};
            vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eAllCommands;
            submitInfo.setPNext(&timelineInfo)
                .setWaitSemaphoreCount(1)
                .setPWaitSemaphores(&timelineSemaphore)
                .setPWaitDstStageMask(&waitStage)
                .setSignalSemaphoreCount(1)
                .setPSignalSemaphores(&timelineSemaphore);

            vkRet = vkQueue.submit(1, &submitInfo, vk::Fence{});
            if (vkRet != vk::Result::eSuccess) {
                NVRHI_ASSERT(0);
                return nvrhi::FE_GENERIC_ERROR;
            }
            transaction.waitValue = signalValue;
        } break;
    }

    return nvrhi::FS_OK;
}

GPAndNVRHIInteropDevice::GPAndNVRHIInteropDevice(nvrhi::IDevice* nvrhiDevice,
                                                 gp::IDevice* gpDevice)
    : m_nvrhiDevice(nvrhiDevice), m_gpDevice(gpDevice), m_transactions{} {
    m_nvrhiDevice->AddRef();
    m_gpDevice->AddRef();
}

GPAndNVRHIInteropDevice::~GPAndNVRHIInteropDevice() {
    switch (m_nvrhiDevice->getGraphicsAPI()) {
        case nvrhi::GraphicsAPI::D3D11:
            nvrhi::SafeRelease(m_transactions[0].nvrhiSemaphore.d3d11Fence);
            break;
        case nvrhi::GraphicsAPI::D3D12:
            for (int i = 0; i < (int)nvrhi::CommandQueue::Count; ++i)
                nvrhi::SafeRelease(m_transactions[i].nvrhiSemaphore.d3d12Fence);
            break;
        case nvrhi::GraphicsAPI::VULKAN: {
            vk::Device vkDevice{
                (VkDevice)m_nvrhiDevice->getNativeObject(nvrhi::ObjectTypes::VK_Device)};
            for (int i = 0; i < (int)nvrhi::CommandQueue::Count; ++i) {
                vkDevice.destroySemaphore(
                    vk::Semaphore{m_transactions[i].nvrhiSemaphore.vkSemaphore});
                m_transactions[i].nvrhiSemaphore.vkSemaphore = 0;
            }
        } break;
    }

    for (int i = 0; i < (int)nvrhi::CommandQueue::Count; ++i)
        nvrhi::SafeRelease(m_transactions[i].gpSemaphore);

    m_nvrhiDevice->Release();
    m_gpDevice->Release();
}

nvrhi::FRESULT GPAndNVRHIInteropDevice::init() {
    auto GAPI = m_nvrhiDevice->getGraphicsAPI();
    switch (GAPI) {
        case nvrhi::GraphicsAPI::D3D11: {
            auto d3d11Device = (ID3D11Device*)m_nvrhiDevice->getNativeObject(
                nvrhi::ObjectTypes::D3D11_Device);
            nvrhi::AutoPtr<ID3D11Device5> d3d11Device5;
            HRESULT hr;
            nvrhi::FRESULT fr;

            if (FAILED(hr = d3d11Device->QueryInterface(IID_PPV_ARGS(&d3d11Device5))))
                return nvrhi::FE_GENERIC_ERROR;

            nvrhi::AutoPtr<ID3D11Fence> d3d11Fence;
            if (FAILED(hr = d3d11Device5->CreateFence(
                           0, D3D11_FENCE_FLAG_SHARED,
                           IID_PPV_ARGS(&d3d11Fence))))
                return nvrhi::FE_GENERIC_ERROR;

            nvrhi::AutoPtr<gp::IGraphicsInteropSemaphore> gpFence;
            if(NVRHI_FAILED(fr = m_gpDevice->createInteropD3D11Fence(d3d11Fence, &gpFence)))
                return fr;

            m_transactions[0].nvrhiSemaphore.d3d11Fence = d3d11Fence;
            d3d11Fence->AddRef();
            m_transactions[0].gpSemaphore = gpFence;
            gpFence->AddRef();
        }
        break;
        case nvrhi::GraphicsAPI::D3D12: {
            auto d3d12Device =
                (ID3D12Device *)m_nvrhiDevice->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);

            HRESULT hr;
            nvrhi::FRESULT fr;

            nvrhi::AutoPtr<ID3D12Fence> d3d12Fences[3];
            nvrhi::AutoPtr<gp::IGraphicsInteropSemaphore> gpFences[3];
            for (int i = 0; i < (int)nvrhi::CommandQueue::Count; ++i) {
                if (FAILED(hr = d3d12Device->CreateFence(
                               0, D3D12_FENCE_FLAG_SHARED,
                               IID_PPV_ARGS(&d3d12Fences[i]))))
                    return nvrhi::FE_GENERIC_ERROR;

                if (NVRHI_FAILED(fr = m_gpDevice->createInteropD3D12Fence(d3d12Fences[i],
                                                                     &gpFences[i])))
                    return fr;
            }

            for (int i = 0; i < (int)nvrhi::CommandQueue::Count; ++i) {
                m_transactions[i].nvrhiSemaphore.d3d12Fence = d3d12Fences[i];
                d3d12Fences[i]->AddRef();
                m_transactions[i].gpSemaphore = gpFences[i];
                gpFences[i]->AddRef();
            }
        } break;
        case nvrhi::GraphicsAPI::VULKAN: {
            auto _vkDevice =
                (VkDevice)m_nvrhiDevice->getNativeObject(nvrhi::ObjectTypes::VK_Device);
            auto vkDevice = vk::Device(_vkDevice);

            vk::SemaphoreTypeCreateInfo timelineCreateInfo;
            timelineCreateInfo.setSemaphoreType(vk::SemaphoreType::eTimeline)
                .setInitialValue(0);

            vk::ExportSemaphoreCreateInfo exportSemaphoreCreateInfo;
#if _WIN32
            exportSemaphoreCreateInfo.setHandleTypes(
                vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueWin32);
#elif defined(__unix__) || defined(__linux__)
            exportSemaphoreCreateInfo.setHandleTypes(
                vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd);
#else
#error "Unsupported platform!"
#endif

            vk::SemaphoreCreateInfo createInfo;
            createInfo.setPNext(&timelineCreateInfo);
            timelineCreateInfo.setPNext(&exportSemaphoreCreateInfo);

            vk::Semaphore vkSemaphores[3];
            vk::Result vkRet;
            nvrhi::FRESULT fr;
            nvrhi::AutoPtr<gp::IGraphicsInteropSemaphore> gpSemaphores[3];

            for (int i = 0; i < (int)nvrhi::CommandQueue::Count; ++i) {
                vkRet = vkDevice.createSemaphore(&createInfo, nullptr, &vkSemaphores[i]);
                if(vkRet != vk::Result::eSuccess) {
                    NVRHI_ASSERT(0);
                    return nvrhi::FE_GENERIC_ERROR;
                }
                if (NVRHI_FAILED(fr = m_gpDevice->createInteropVulkanSemaphore(
                                (void*)(VkSemaphore)vkSemaphores[i],
                                (void*)(VkDevice)vkDevice, &gpSemaphores[i]))) {
                    NVRHI_ASSERT(0);
                    return nvrhi::FE_GENERIC_ERROR;
                }
            }

            for (int i = 0; i < (int)nvrhi::CommandQueue::Count; ++i) {
                m_transactions[i].nvrhiSemaphore.vkSemaphore = (VkSemaphore)vkSemaphores[i];
                m_transactions[i].gpSemaphore = gpSemaphores[i];
                gpSemaphores[i]->AddRef();
            }
        }
        break;
    }

    return nvrhi::FS_OK;
}

nvrhi::FRESULT createGPAndNVRHIDevice(nvrhi::IDevice* nvrhiDevice, gp::IDevice* gpDevice,
                               IGPAndNVRHIInteropDevice** _interopDevice) {
    if (!nvrhiDevice || !gpDevice) return nvrhi::FE_INVALID_ARGS;

    auto interopDevice = nvrhi::TakeOver(MAKE_RC_OBJ(GPAndNVRHIInteropDevice, nvrhiDevice, gpDevice));
    nvrhi::FRESULT fr = interopDevice->init();
    if (NVRHI_FAILED(fr)) return fr;

    if(_interopDevice) {
        *_interopDevice = interopDevice;
        interopDevice->AddRef();
    }
    return nvrhi::FS_OK;
}

}  // namespace donut
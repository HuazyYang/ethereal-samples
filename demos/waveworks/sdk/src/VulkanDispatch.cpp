// The SDK owns an NVRHI Vulkan backend independently of the demo device manager.
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
#include <Windows.h>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

bool InitializeWaveWorksVulkanDispatch(VkInstance instance, VkDevice device)
{
    // The host owns the loader and native objects; the SDK owns its dispatch table.
    auto loader = GetModuleHandleW(L"vulkan-1.dll");
    if (!loader) return false;
    auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(loader, "vkGetInstanceProcAddr"));
    if (!getProc) return false;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(getProc);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(instance));
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(device));
    return true;
}

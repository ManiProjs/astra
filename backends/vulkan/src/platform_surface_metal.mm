#import <QuartzCore/CAMetalLayer.h>

#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_metal.h>

#include "platform_surface.hpp"

bool astra_vulkan_create_platform_surface(VkInstance instance, void* native_surface,
                                          VkSurfaceKHR* surface, std::string& error) {
    if (!native_surface) {
        error = "Vulkan requires the AppKit window's CAMetalLayer.";
        return false;
    }
    const auto create_surface = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT"));
    if (!create_surface) {
        error = "VK_EXT_metal_surface is advertised but vkCreateMetalSurfaceEXT is unavailable.";
        return false;
    }
    VkMetalSurfaceCreateInfoEXT create_info{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
    create_info.pLayer = (__bridge CAMetalLayer*)native_surface;
    const auto result = create_surface(instance, &create_info, nullptr, surface);
    if (result != VK_SUCCESS) {
        error = "vkCreateMetalSurfaceEXT failed with VkResult " + std::to_string(result) + '.';
        return false;
    }
    return true;
}

void astra_vulkan_get_framebuffer_size(void* native_surface, std::uint32_t& width,
                                       std::uint32_t& height) noexcept {
    if (!native_surface) {
        width = height = 0;
        return;
    }
    const auto layer = (__bridge CAMetalLayer*)native_surface;
    const auto size = layer.drawableSize;
    width = size.width > 0.0 ? static_cast<std::uint32_t>(size.width) : 0U;
    height = size.height > 0.0 ? static_cast<std::uint32_t>(size.height) : 0U;
}

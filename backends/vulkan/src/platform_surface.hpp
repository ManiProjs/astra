#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

[[nodiscard]] bool astra_vulkan_create_platform_surface(
    VkInstance instance, void* native_surface, VkSurfaceKHR* surface, std::string& error);

void astra_vulkan_get_framebuffer_size(void* native_surface, std::uint32_t& width,
                                       std::uint32_t& height) noexcept;

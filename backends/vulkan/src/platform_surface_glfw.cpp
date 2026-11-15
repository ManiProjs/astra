#include "platform_surface.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

bool astra_vulkan_create_platform_surface(VkInstance instance, void* native_surface,
                                          VkSurfaceKHR* surface, std::string& error) {
    if (!native_surface) {
        error = "Vulkan requires a GLFW window created with WindowClientAPI::vulkan.";
        return false;
    }
    const auto result = glfwCreateWindowSurface(instance, static_cast<GLFWwindow*>(native_surface),
                                                nullptr, surface);
    if (result != VK_SUCCESS) {
        error = "glfwCreateWindowSurface failed with VkResult " + std::to_string(result) + '.';
        return false;
    }
    return true;
}

void astra_vulkan_get_framebuffer_size(void* native_surface, std::uint32_t& width,
                                       std::uint32_t& height) noexcept {
    int pixel_width{}, pixel_height{};
    glfwGetFramebufferSize(static_cast<GLFWwindow*>(native_surface), &pixel_width, &pixel_height);
    width = pixel_width > 0 ? static_cast<std::uint32_t>(pixel_width) : 0U;
    height = pixel_height > 0 ? static_cast<std::uint32_t>(pixel_height) : 0U;
}

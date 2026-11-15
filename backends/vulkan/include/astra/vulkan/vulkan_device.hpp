#pragma once

#include <astra/rhi/render_device.hpp>

#include <expected>
#include <memory>

namespace astra::vulkan {

// On Windows/Linux, native_surface is a GLFWwindow* created with
// WindowClientAPI::vulkan. On macOS it is the AppKit CAMetalLayer*.
[[nodiscard]] std::expected<std::unique_ptr<rhi::RenderDevice>, rhi::RenderError>
create_vulkan_render_device(void* native_surface);

} // namespace astra::vulkan

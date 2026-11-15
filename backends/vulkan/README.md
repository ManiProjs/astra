# Vulkan backend

The initial Vulkan backend is a real clear/present vertical slice implementing the live
`astra::rhi::RenderDevice` contract. It creates a Vulkan instance, native presentation surface,
physical/logical device, graphics/present queue, swapchain, image views, render pass, framebuffers,
command pool/buffers, semaphores and fence. `render_frame` acquires a swapchain image, records a
clear render pass, submits it, and presents it. Swapchain out-of-date/suboptimal results trigger
recreation.

## Native surfaces

- Windows and Linux: create `astra::platform::Window` with
  `WindowConfig::client_api = WindowClientAPI::vulkan`. This creates a GLFW `GLFW_NO_API` window;
  the Vulkan backend calls `glfwCreateWindowSurface` on its `GLFWwindow*`.
- macOS: the existing AppKit window continues to expose its `CAMetalLayer*`; Vulkan uses
  `VK_EXT_metal_surface` and `vkCreateMetalSurfaceEXT`. MoltenVK must be installed and must
  advertise the extension at runtime. The Metal renderer and its default window path are unchanged.

The backend is compiled only when `ASTRA_ENABLE_VULKAN` is enabled, CMake finds `Vulkan`, and a
native surface adapter is available. Windows/Linux additionally require the existing GLFW
platform dependency. macOS requires the Vulkan loader/SDK and runtime `VK_EXT_metal_surface`.
When the SDK is missing, other backends continue to configure and build without Vulkan.

## Deliberate scope

This is not renderer parity with Metal. GPU mesh/virtual-geometry/texture/environment creation and
all scene/mesh drawing return `RenderErrorCode::unsupported_backend`. Only `render_frame(clear)`
is implemented. Shader compilation, descriptors, graphics pipelines, depth, and resource upload
are not part of this slice. `Renderer::create` reports unsupported when Vulkan was not built and
reports an invalid surface when a GLFW window was created for a different client API.

Example for GLFW platforms:

```cpp
auto window = astra::platform::Window::create({
    .title = "Astra Vulkan",
    .width = 1280,
    .height = 720,
    .client_api = astra::platform::WindowClientAPI::vulkan,
});
if (!window) return 1;

auto renderer = astra::renderer::Renderer::create(
    *window, {.backend = astra::rhi::Backend::vulkan});
if (!renderer) return 1;
while (window->poll_events()) {
    if (auto frame = renderer->render_frame({0.025F, 0.03F, 0.04F, 1.0F}); !frame)
        break;
}
```

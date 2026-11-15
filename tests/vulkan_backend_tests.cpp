#include <astra/platform/window.hpp>
#include <astra/renderer/renderer.hpp>
#include <astra/vulkan/vulkan_device.hpp>

#include <array>
#include <cstdio>
#include <type_traits>

int main() {
    using Factory = decltype(&astra::vulkan::create_vulkan_render_device);
    static_assert(std::is_same_v<
                  Factory,
                  std::expected<std::unique_ptr<astra::rhi::RenderDevice>, astra::rhi::RenderError> (*)(void*)>);

    const auto result = astra::vulkan::create_vulkan_render_device(nullptr);
    if (result || result.error().code != astra::rhi::RenderErrorCode::invalid_surface)
        return 1;

    auto window = astra::platform::Window::create({
        .title = "Astra Vulkan backend contract test",
        .width = 64,
        .height = 64,
        .client_api = astra::platform::WindowClientAPI::vulkan,
        .visible = false,
    });
    // A display server is not guaranteed on headless Linux CI workers.
    if (!window)
        return 77;

    auto renderer = astra::renderer::Renderer::create(
        *window, {.backend = astra::rhi::Backend::vulkan});
    if (!renderer) {
        if (renderer.error().code == astra::rhi::RenderErrorCode::unsupported_backend ||
            renderer.error().code == astra::rhi::RenderErrorCode::device_unavailable) {
            std::fprintf(stderr, "SKIP: Vulkan runtime unavailable: %s\n",
                         renderer.error().message.c_str());
            return 77;
        }
        std::fprintf(stderr, "Vulkan renderer creation failed: %s\n",
                     renderer.error().message.c_str());
        return 1;
    }
    if (renderer->backend() != astra::rhi::Backend::vulkan || renderer->device_name().empty())
        return 1;

    if (auto frame = renderer->render_frame({0.1F, 0.15F, 0.2F, 1.0F}); !frame) {
        std::fprintf(stderr, "Vulkan clear/present failed: %s\n", frame.error().message.c_str());
        return 1;
    }
    if (auto mesh = renderer->create_mesh({});
        mesh || mesh.error().code != astra::rhi::RenderErrorCode::unsupported_backend)
        return 1;
    if (auto texture = renderer->create_texture({});
        texture || texture.error().code != astra::rhi::RenderErrorCode::unsupported_backend)
        return 1;
    if (auto geometry = renderer->create_virtual_geometry({});
        geometry || geometry.error().code != astra::rhi::RenderErrorCode::unsupported_backend)
        return 1;
    if (auto scene = renderer->render_scene(
            std::array{astra::rhi::DrawItem{}}, astra::rhi::Matrix4{});
        scene || scene.error().code != astra::rhi::RenderErrorCode::unsupported_backend)
        return 1;
    const std::array<float, 4> environment_pixels{0.0F, 0.0F, 0.0F, 1.0F};
    const astra::rhi::EnvironmentData environment{
        .width = 1, .height = 1, .rgba32f = environment_pixels};
    if (auto created = renderer->create_environment(environment);
        created || created.error().code != astra::rhi::RenderErrorCode::unsupported_backend)
        return 1;
    return 0;
}

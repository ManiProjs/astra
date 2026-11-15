#include <astra/renderer/renderer.hpp>

#if defined(ASTRA_HAS_METAL)
#include <astra/metal/metal_device.hpp>
#endif
#if defined(ASTRA_HAS_OPENGL)
#include <astra/opengl/opengl_device.hpp>
#endif
#if defined(ASTRA_HAS_VULKAN)
#include <astra/vulkan/vulkan_device.hpp>
#endif

#include <utility>

namespace astra::renderer {

Renderer::Renderer(std::unique_ptr<rhi::RenderDevice> device) noexcept
    : device_(std::move(device)) {}

Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

std::expected<Renderer, rhi::RenderError> Renderer::create(platform::Window& window,
                                                           RendererConfig config) {
    if (!window.is_open() || window.native_render_surface() == nullptr) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::invalid_surface,
            .message = "Renderer creation requires an open native window.",
        });
    }

    switch (config.backend) {
    case rhi::Backend::automatic:
#if defined(ASTRA_HAS_METAL)
        config.backend = rhi::Backend::metal;
#elif defined(ASTRA_HAS_OPENGL)
        config.backend = rhi::Backend::opengl;
#else
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::unsupported_backend,
            .message = "No default graphics backend is available on this platform.",
        });
#endif
        [[fallthrough]];
    case rhi::Backend::metal:
#if defined(ASTRA_HAS_METAL)
        if (auto device =
                astra::metal::create_metal_render_device(window.native_render_surface())) {
            return Renderer{std::move(*device)};
        } else {
            return std::unexpected(device.error());
        }
#else
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::unsupported_backend,
            .message = "The Metal backend is only built on Apple platforms.",
        });
#endif
    case rhi::Backend::direct3d9:
    case rhi::Backend::direct3d10:
    case rhi::Backend::direct3d11:
    case rhi::Backend::direct3d12:
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::unsupported_backend,
            .message = std::string{rhi::backend_name(config.backend)} +
                       " is not available in this build yet.",
        });
    case rhi::Backend::opengl:
#if defined(ASTRA_HAS_OPENGL)
#if defined(ASTRA_HAS_GLFW)
        if (window.config().client_api != platform::WindowClientAPI::opengl)
            return std::unexpected(rhi::RenderError{
                .code = rhi::RenderErrorCode::invalid_surface,
                .message = "OpenGL requires a GLFW window created with WindowClientAPI::opengl.",
            });
#endif
        if (auto device =
                astra::opengl::create_opengl_render_device(window.native_render_surface())) {
            return Renderer{std::move(*device)};
        } else {
            return std::unexpected(device.error());
        }
#else
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::unsupported_backend,
            .message = "OpenGL is not available in this build.",
        });
#endif
    case rhi::Backend::vulkan:
#if defined(ASTRA_HAS_VULKAN)
#if defined(ASTRA_HAS_GLFW)
        if (window.config().client_api != platform::WindowClientAPI::vulkan)
            return std::unexpected(rhi::RenderError{
                .code = rhi::RenderErrorCode::invalid_surface,
                .message = "Vulkan requires a GLFW window created with WindowClientAPI::vulkan (no client API).",
            });
#endif
        if (auto device =
                astra::vulkan::create_vulkan_render_device(window.native_render_surface())) {
            return Renderer{std::move(*device)};
        } else {
            return std::unexpected(device.error());
        }
#else
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::unsupported_backend,
            .message = "Vulkan is not available in this build (Vulkan SDK/loader or native WSI support missing).",
        });
#endif
    }

    return std::unexpected(rhi::RenderError{
        .code = rhi::RenderErrorCode::unsupported_backend,
        .message = "Unknown graphics backend.",
    });
}

std::expected<void, rhi::RenderError> Renderer::render_frame(rhi::Color clear_color) {
    if (!device_) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::device_unavailable,
            .message = "The renderer has no initialized device.",
        });
    }
    return device_->render_frame(clear_color);
}

std::expected<std::unique_ptr<rhi::GpuMesh>, rhi::RenderError>
Renderer::create_mesh(const rhi::MeshData& data) {
    if (!device_) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::device_unavailable,
            .message = "The renderer has no initialized device.",
        });
    }
    return device_->create_mesh(data);
}

std::expected<std::unique_ptr<rhi::GpuVirtualGeometry>, rhi::RenderError>
Renderer::create_virtual_geometry(const rhi::VirtualGeometryData& data) {
    if (!device_) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::device_unavailable,
            .message = "The renderer has no initialized device.",
        });
    }
    return device_->create_virtual_geometry(data);
}

std::expected<std::unique_ptr<rhi::GpuTexture>, rhi::RenderError>
Renderer::create_texture(const rhi::TextureData& data) {
    if (!device_) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::device_unavailable,
            .message = "The renderer has no initialized device.",
        });
    }
    return device_->create_texture(data);
}

std::expected<std::unique_ptr<rhi::GpuEnvironment>, rhi::RenderError>
Renderer::create_environment(const rhi::EnvironmentData& data) {
    if (!device_) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::device_unavailable,
            .message = "The renderer has no initialized device.",
        });
    }
    if (const auto valid = rhi::validate_environment(data); !valid) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::invalid_scene,
            .message = valid.error(),
        });
    }
    return device_->create_environment(data);
}

std::expected<void, rhi::RenderError> Renderer::render_mesh(const rhi::GpuMesh& mesh,
                                                            const rhi::Matrix4& view_projection,
                                                            const rhi::Matrix4& model,
                                                            rhi::Color clear_color,
                                                            rhi::Vec3 light_direction) {
    const rhi::DrawItem item{.mesh = &mesh, .model = model};
    return render_scene(std::span<const rhi::DrawItem>{&item, 1}, view_projection, clear_color,
                        rhi::SceneSettings{.light_direction = light_direction});
}

std::expected<void, rhi::RenderError>
Renderer::render_scene(std::span<const rhi::DrawItem> draw_items,
                       const rhi::Matrix4& view_projection, rhi::Color clear_color,
                       rhi::SceneSettings settings) {
    if (!device_) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::device_unavailable,
            .message = "The renderer has no initialized device.",
        });
    }
    if (const auto valid = rhi::validate_scene_lighting(settings); !valid) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::invalid_scene,
            .message = valid.error(),
        });
    }
    return device_->render_scene(clear_color, draw_items, view_projection, settings);
}

rhi::Backend Renderer::backend() const noexcept {
    return device_ ? device_->backend() : rhi::Backend::automatic;
}

const std::string& Renderer::device_name() const noexcept {
    static const std::string unavailable{"Unavailable"};
    return device_ ? device_->device_name() : unavailable;
}

double Renderer::last_gpu_frame_time_ms() const noexcept {
    return device_ ? device_->last_gpu_frame_time_ms() : 0.0;
}

} // namespace astra::renderer

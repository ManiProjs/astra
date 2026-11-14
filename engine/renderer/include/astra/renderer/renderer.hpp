#pragma once

#include <astra/platform/window.hpp>
#include <astra/rhi/render_device.hpp>

#include <expected>
#include <memory>
#include <string>

namespace astra::renderer {

struct RendererConfig final {
    rhi::Backend backend{rhi::Backend::automatic};
};

class Renderer final {
  public:
    [[nodiscard]] static std::expected<Renderer, rhi::RenderError>
    create(platform::Window& window, RendererConfig config = {});

    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;

    [[nodiscard]] std::expected<void, rhi::RenderError>
    render_frame(rhi::Color clear_color = {0.035F, 0.045F, 0.065F, 1.0F});

    [[nodiscard]] std::expected<std::unique_ptr<rhi::GpuMesh>, rhi::RenderError>
    create_mesh(const rhi::MeshData& data);

    [[nodiscard]] std::expected<std::unique_ptr<rhi::GpuVirtualGeometry>, rhi::RenderError>
    create_virtual_geometry(const rhi::VirtualGeometryData& data);

    [[nodiscard]] std::expected<std::unique_ptr<rhi::GpuTexture>, rhi::RenderError>
    create_texture(const rhi::TextureData& data);

    [[nodiscard]] std::expected<std::unique_ptr<rhi::GpuEnvironment>, rhi::RenderError>
    create_environment(const rhi::EnvironmentData& data);

    [[nodiscard]] std::expected<void, rhi::RenderError>
    render_mesh(const rhi::GpuMesh& mesh, const rhi::Matrix4& view_projection,
                const rhi::Matrix4& model, rhi::Color clear_color = {0.035F, 0.045F, 0.065F, 1.0F},
                rhi::Vec3 light_direction = {-0.45F, -0.8F, -0.35F});

    [[nodiscard]] std::expected<void, rhi::RenderError>
    render_scene(std::span<const rhi::DrawItem> draw_items, const rhi::Matrix4& view_projection,
                 rhi::Color clear_color = {0.035F, 0.045F, 0.065F, 1.0F},
                 rhi::SceneSettings settings = {});

    [[nodiscard]] rhi::Backend backend() const noexcept;
    [[nodiscard]] const std::string& device_name() const noexcept;
    [[nodiscard]] double last_gpu_frame_time_ms() const noexcept;

  private:
    explicit Renderer(std::unique_ptr<rhi::RenderDevice> device) noexcept;

    std::unique_ptr<rhi::RenderDevice> device_;
};

} // namespace astra::renderer

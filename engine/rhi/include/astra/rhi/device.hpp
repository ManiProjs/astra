#pragma once

#include <astra/rhi/command.hpp>
#include <astra/rhi/render_graph.hpp>

#include <expected>
#include <memory>

namespace astra::rhi {

struct DeviceCapabilities final {
    std::uint32_t max_texture_dimension_2d{};
    std::uint32_t max_texture_array_layers{};
    std::uint32_t max_color_attachments{};
    std::uint32_t max_vertex_attributes{};
    std::uint32_t max_vertex_buffers{};
    std::uint32_t max_compute_workgroup_invocations{};
    bool supports_compute{};
    bool supports_indirect_draw{};
    bool supports_timestamp_queries{};
    bool supports_texture_compression{};
};

class Device {
  public:
    virtual ~Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] virtual Backend backend() const noexcept = 0;
    [[nodiscard]] virtual const char* name() const noexcept = 0;
    [[nodiscard]] virtual const DeviceCapabilities& capabilities() const noexcept = 0;
    [[nodiscard]] virtual std::expected<std::unique_ptr<ShaderModule>, Error>
    create_shader_module(const ShaderModuleDesc& description) = 0;
    [[nodiscard]] virtual std::expected<std::unique_ptr<BindGroupLayout>, Error>
    create_bind_group_layout(std::span<const BindingLayoutEntry> entries) = 0;
    [[nodiscard]] virtual std::expected<std::unique_ptr<BindGroup>, Error>
    create_bind_group(const BindGroupDesc& description) = 0;
    [[nodiscard]] virtual std::expected<std::unique_ptr<Buffer>, Error>
    create_buffer(const BufferDesc& description, BufferUpload initial_data = {}) = 0;
    [[nodiscard]] virtual std::expected<std::unique_ptr<Texture>, Error>
    create_texture(const TextureDesc& description,
                   std::span<const TextureSubresourceData> initial_data = {}) = 0;
    [[nodiscard]] virtual std::expected<std::unique_ptr<Sampler>, Error>
    create_sampler(const SamplerDesc& description) = 0;
    [[nodiscard]] virtual std::expected<std::unique_ptr<Pipeline>, Error>
    create_render_pipeline(const RenderPipelineDesc& description) = 0;
    [[nodiscard]] virtual std::expected<std::unique_ptr<Pipeline>, Error>
    create_compute_pipeline(const ComputePipelineDesc& description) = 0;
    [[nodiscard]] virtual std::expected<std::unique_ptr<CommandBuffer>, Error>
    create_command_buffer() = 0;
    [[nodiscard]] virtual std::expected<void, Error>
    submit(std::span<CommandBuffer* const> command_buffers) = 0;

  protected:
    Device() = default;
};

} // namespace astra::rhi

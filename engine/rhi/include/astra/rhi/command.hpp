#pragma once

#include <astra/rhi/pipeline.hpp>
#include <astra/rhi/resource.hpp>
#include <astra/rhi/binding.hpp>

#include <cstdint>
#include <span>

namespace astra::rhi {

enum class LoadOp : std::uint8_t { load, clear, discard };
enum class StoreOp : std::uint8_t { store, discard };

struct ColorAttachment final {
    Texture* texture{};
    std::uint32_t mip_level{};
    std::uint32_t array_layer{};
    LoadOp load{LoadOp::clear};
    StoreOp store{StoreOp::store};
    Color clear_color{};
};

struct DepthAttachment final {
    Texture* texture{};
    LoadOp depth_load{LoadOp::clear};
    StoreOp depth_store{StoreOp::discard};
    float clear_depth{1.0F};
};

struct RenderPassDesc final {
    std::span<const ColorAttachment> colors;
    DepthAttachment depth{};
    Extent2D extent{};
    const char* debug_name{};
};

class CommandEncoder {
  public:
    virtual ~CommandEncoder() = default;
    CommandEncoder(const CommandEncoder&) = delete;
    CommandEncoder& operator=(const CommandEncoder&) = delete;

    [[nodiscard]] virtual ErrorCode begin_render_pass(const RenderPassDesc& description) = 0;
    [[nodiscard]] virtual ErrorCode end_render_pass() = 0;
    [[nodiscard]] virtual ErrorCode set_pipeline(const Pipeline& pipeline) = 0;
    [[nodiscard]] virtual ErrorCode set_bind_group(std::uint32_t slot,
                                                   const BindGroup& group) = 0;
    [[nodiscard]] virtual ErrorCode set_vertex_buffer(std::uint32_t slot, const Buffer& buffer,
                                                      std::size_t offset = 0) = 0;
    [[nodiscard]] virtual ErrorCode set_index_buffer(const Buffer& buffer, IndexFormat format,
                                                     std::size_t offset = 0) = 0;
    [[nodiscard]] virtual ErrorCode set_viewport(Offset2D origin, Extent2D extent,
                                                 float min_depth = 0.0F,
                                                 float max_depth = 1.0F) = 0;
    [[nodiscard]] virtual ErrorCode set_scissor(Offset2D origin, Extent2D extent) = 0;
    [[nodiscard]] virtual ErrorCode draw(std::uint32_t vertex_count,
                                         std::uint32_t instance_count = 1,
                                         std::uint32_t first_vertex = 0,
                                         std::uint32_t first_instance = 0) = 0;
    [[nodiscard]] virtual ErrorCode draw_indexed(std::uint32_t index_count,
                                                 std::uint32_t instance_count = 1,
                                                 std::uint32_t first_index = 0,
                                                 std::int32_t vertex_offset = 0,
                                                 std::uint32_t first_instance = 0) = 0;
    [[nodiscard]] virtual ErrorCode dispatch(std::uint32_t groups_x, std::uint32_t groups_y,
                                             std::uint32_t groups_z) = 0;

  protected:
    CommandEncoder() = default;
};

class CommandBuffer {
  public:
    virtual ~CommandBuffer() = default;
    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;
    [[nodiscard]] virtual CommandEncoder& begin() = 0;
    [[nodiscard]] virtual ErrorCode finish() = 0;

  protected:
    CommandBuffer() = default;
};

} // namespace astra::rhi

#pragma once

#include <astra/rhi/descriptors.hpp>

#include <cstdint>
#include <cstddef>
#include <span>

namespace astra::rhi {

enum class ShaderStage : std::uint8_t { vertex, fragment, compute };
enum class PrimitiveTopology : std::uint8_t { point_list, line_list, line_strip, triangle_list, triangle_strip };
enum class IndexFormat : std::uint8_t { uint16, uint32 };
enum class PolygonMode : std::uint8_t { fill, line, point };
enum class CullMode : std::uint8_t { none, front, back };
enum class FrontFace : std::uint8_t { clockwise, counter_clockwise };
enum class CompareOp : std::uint8_t { never, less, equal, less_equal, greater, not_equal, greater_equal, always };
enum class BlendFactor : std::uint8_t { zero, one, source_alpha, one_minus_source_alpha, destination_alpha, one_minus_destination_alpha };
enum class BlendOp : std::uint8_t { add, subtract, reverse_subtract, minimum, maximum };

class ShaderModule {
  public:
    virtual ~ShaderModule() = default;
    ShaderModule(const ShaderModule&) = delete;
    ShaderModule& operator=(const ShaderModule&) = delete;

  protected:
    ShaderModule() = default;
};

struct ShaderModuleDesc final {
    ShaderStage stage{ShaderStage::vertex};
    std::span<const std::byte> code;
    const char* entry_point{"main"};
    const char* debug_name{};
};

struct VertexAttribute final {
    std::uint32_t location{};
    std::uint32_t buffer_slot{};
    std::uint32_t offset{};
    PixelFormat format{PixelFormat::undefined};
};

struct VertexBufferLayout final {
    std::uint32_t slot{};
    std::uint32_t stride{};
    bool per_instance{};
};

struct RasterState final {
    PolygonMode polygon_mode{PolygonMode::fill};
    CullMode cull_mode{CullMode::back};
    FrontFace front_face{FrontFace::counter_clockwise};
    bool depth_clip{true};
};

struct DepthStencilState final {
    bool depth_test{};
    bool depth_write{};
    CompareOp depth_compare{CompareOp::less};
    bool stencil_test{};
};

struct ColorBlendState final {
    bool enabled{};
    BlendFactor source_color{BlendFactor::one};
    BlendFactor destination_color{BlendFactor::zero};
    BlendOp color_operation{BlendOp::add};
    BlendFactor source_alpha{BlendFactor::one};
    BlendFactor destination_alpha{BlendFactor::zero};
    BlendOp alpha_operation{BlendOp::add};
    std::uint8_t write_mask{0x0f};
};

struct RenderPipelineDesc final {
    const ShaderModule* vertex_shader{};
    const ShaderModule* fragment_shader{};
    std::span<const VertexAttribute> vertex_attributes;
    std::span<const VertexBufferLayout> vertex_buffers;
    std::span<const PixelFormat> color_formats;
    PixelFormat depth_format{PixelFormat::undefined};
    SampleCount samples{SampleCount::x1};
    PrimitiveTopology topology{PrimitiveTopology::triangle_list};
    RasterState raster{};
    DepthStencilState depth_stencil{};
    ColorBlendState blend{};
    const char* debug_name{};
};

struct ComputePipelineDesc final {
    const ShaderModule* compute_shader{};
    const char* debug_name{};
};

class Pipeline {
  public:
    virtual ~Pipeline() = default;
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

  protected:
    Pipeline() = default;
};

} // namespace astra::rhi

#pragma once

#include <astra/rhi/types.hpp>

#include <cstddef>
#include <cstdint>

namespace astra::rhi {

enum class PixelFormat : std::uint8_t {
    undefined,
    r8_unorm,
    rg8_unorm,
    rgba8_unorm,
    rgba8_srgb,
    bgra8_unorm,
    bgra8_srgb,
    r16_float,
    rg16_float,
    rgba16_float,
    r32_float,
    rg32_float,
    rgba32_float,
    depth16_unorm,
    depth32_float,
    depth24_stencil8,
};

enum class TextureDimension : std::uint8_t { texture_2d, texture_2d_array, texture_3d, cube };
enum class SampleCount : std::uint8_t { x1 = 1, x2 = 2, x4 = 4, x8 = 8 };

enum class TextureUsage : std::uint16_t {
    none = 0,
    sampled = 1U << 0U,
    storage = 1U << 1U,
    color_attachment = 1U << 2U,
    depth_stencil_attachment = 1U << 3U,
    transfer_source = 1U << 4U,
    transfer_destination = 1U << 5U,
};

[[nodiscard]] constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) noexcept {
    return static_cast<TextureUsage>(static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b));
}
[[nodiscard]] constexpr TextureUsage operator&(TextureUsage a, TextureUsage b) noexcept {
    return static_cast<TextureUsage>(static_cast<std::uint16_t>(a) & static_cast<std::uint16_t>(b));
}
[[nodiscard]] constexpr bool contains(TextureUsage set, TextureUsage value) noexcept {
    return (set & value) == value;
}

struct TextureDesc final {
    TextureDimension dimension{TextureDimension::texture_2d};
    Extent2D extent{};
    std::uint32_t depth_or_layers{1};
    std::uint32_t mip_levels{1};
    SampleCount samples{SampleCount::x1};
    PixelFormat format{PixelFormat::rgba8_unorm};
    TextureUsage usage{TextureUsage::sampled};
    const char* debug_name{};
};

enum class BufferUsage : std::uint16_t {
    none = 0,
    vertex = 1U << 0U,
    index = 1U << 1U,
    uniform = 1U << 2U,
    storage = 1U << 3U,
    indirect = 1U << 4U,
    transfer_source = 1U << 5U,
    transfer_destination = 1U << 6U,
};

[[nodiscard]] constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) noexcept {
    return static_cast<BufferUsage>(static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b));
}
[[nodiscard]] constexpr BufferUsage operator&(BufferUsage a, BufferUsage b) noexcept {
    return static_cast<BufferUsage>(static_cast<std::uint16_t>(a) & static_cast<std::uint16_t>(b));
}
[[nodiscard]] constexpr bool contains(BufferUsage set, BufferUsage value) noexcept {
    return (set & value) == value;
}

struct BufferDesc final {
    std::size_t size{};
    BufferUsage usage{BufferUsage::none};
    bool host_visible{};
    bool host_coherent{};
    const char* debug_name{};
};

enum class Filter : std::uint8_t { nearest, linear };
enum class MipFilter : std::uint8_t { none, nearest, linear };
enum class AddressMode : std::uint8_t { repeat, mirrored_repeat, clamp_to_edge, clamp_to_border };

struct SamplerDesc final {
    Filter min_filter{Filter::linear};
    Filter mag_filter{Filter::linear};
    MipFilter mip_filter{MipFilter::linear};
    AddressMode address_u{AddressMode::repeat};
    AddressMode address_v{AddressMode::repeat};
    AddressMode address_w{AddressMode::repeat};
    float min_lod{};
    float max_lod{1000.0F};
    float max_anisotropy{1.0F};
};

} // namespace astra::rhi

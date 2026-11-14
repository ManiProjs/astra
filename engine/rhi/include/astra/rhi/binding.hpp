#pragma once

#include <astra/rhi/resource.hpp>

#include <cstdint>
#include <span>

namespace astra::rhi {

enum class BindingType : std::uint8_t {
    uniform_buffer,
    storage_buffer,
    sampled_texture,
    storage_texture,
    sampler,
};

enum class ShaderVisibility : std::uint8_t {
    none = 0,
    vertex = 1U << 0U,
    fragment = 1U << 1U,
    compute = 1U << 2U,
    all_graphics = (1U << 0U) | (1U << 1U),
    all = (1U << 0U) | (1U << 1U) | (1U << 2U),
};

[[nodiscard]] constexpr ShaderVisibility operator|(ShaderVisibility a, ShaderVisibility b) noexcept {
    return static_cast<ShaderVisibility>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

struct BindingLayoutEntry final {
    std::uint32_t binding{};
    BindingType type{BindingType::uniform_buffer};
    std::uint32_t count{1};
    ShaderVisibility visibility{ShaderVisibility::all};
};

struct BindingResource final {
    std::uint32_t binding{};
    std::uint32_t array_element{};
    Resource* resource{};
    Sampler* sampler{};
    std::size_t offset{};
    std::size_t size{};
};

class BindGroupLayout {
  public:
    virtual ~BindGroupLayout() = default;
    BindGroupLayout(const BindGroupLayout&) = delete;
    BindGroupLayout& operator=(const BindGroupLayout&) = delete;

  protected:
    BindGroupLayout() = default;
};

class BindGroup {
  public:
    virtual ~BindGroup() = default;
    BindGroup(const BindGroup&) = delete;
    BindGroup& operator=(const BindGroup&) = delete;

  protected:
    BindGroup() = default;
};

struct BindGroupDesc final {
    const BindGroupLayout* layout{};
    std::span<const BindingResource> resources;
    const char* debug_name{};
};

} // namespace astra::rhi

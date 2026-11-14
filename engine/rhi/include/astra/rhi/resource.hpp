#pragma once

#include <astra/rhi/descriptors.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace astra::rhi {

class Resource {
  public:
    virtual ~Resource() = default;
    Resource(const Resource&) = delete;
    Resource& operator=(const Resource&) = delete;
    [[nodiscard]] virtual std::uint64_t byte_size() const noexcept = 0;

  protected:
    Resource() = default;
};

class Buffer : public Resource {
  public:
    [[nodiscard]] virtual const BufferDesc& description() const noexcept = 0;
    [[nodiscard]] virtual std::expected<void, Error>
    write(std::size_t offset, std::span<const std::byte> data) = 0;

  protected:
    Buffer() = default;
};

class Texture : public Resource {
  public:
    [[nodiscard]] virtual const TextureDesc& description() const noexcept = 0;

  protected:
    Texture() = default;
};

class Sampler {
  public:
    virtual ~Sampler() = default;
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

  protected:
    Sampler() = default;
};

struct BufferUpload final {
    std::span<const std::byte> bytes;
};

struct TextureSubresourceData final {
    std::uint32_t mip_level{};
    std::uint32_t array_layer{};
    std::size_t row_pitch{};
    std::size_t slice_pitch{};
    std::span<const std::byte> bytes;
};

} // namespace astra::rhi

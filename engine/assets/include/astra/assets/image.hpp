#pragma once

#include <astra/assets/gltf.hpp>

#include <expected>
#include <span>
#include <string_view>

namespace astra::assets {

struct DecodedImage final {
    std::uint32_t width{};
    std::uint32_t height{};
    // Top-to-bottom, tightly packed RGBA8.
    std::vector<std::byte> rgba8;
};

// Decodes common glTF image payloads (PNG/JPEG) using the native platform image codec.
[[nodiscard]] std::expected<DecodedImage, Error>
decode_image(std::span<const std::byte> encoded, std::string_view mime_type = {});

} // namespace astra::assets

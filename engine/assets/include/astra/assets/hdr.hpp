#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace astra::assets {

struct HdrImage final {
    std::uint32_t width{};
    std::uint32_t height{};
    // Top-to-bottom, left-to-right, tightly packed linear RGBA32F.
    // RGB contains decoded Radiance values; alpha is always 1.
    std::vector<float> rgba32f;
};

struct HdrDecodeError final {
    std::string message;
};

struct HdrDecodeOptions final {
    std::size_t maximum_output_bytes{512U * 1024U * 1024U};
};

// Decodes a Radiance RGBE (.hdr) image from memory. No file access is performed.
[[nodiscard]] std::expected<HdrImage, HdrDecodeError>
decode_hdr(std::span<const std::byte> encoded, HdrDecodeOptions options = {});

} // namespace astra::assets

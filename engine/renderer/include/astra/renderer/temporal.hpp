#pragma once

#include <astra/rhi/types.hpp>

#include <cstddef>
#include <cstdint>

namespace astra::renderer {

// Sub-pixel projection offset in normalized device coordinates. The sequence
// repeats after eight frames to bound its phase and avoid long-term drift.
struct TemporalJitter final {
    float x{};
    float y{};
};

[[nodiscard]] inline float halton(std::uint32_t index, std::uint32_t base) noexcept {
    if (base < 2U)
        return 0.0F;
    float result = 0.0F;
    float fraction = 1.0F / static_cast<float>(base);
    while (index > 0U) {
        result += fraction * static_cast<float>(index % base);
        index /= base;
        fraction /= static_cast<float>(base);
    }
    return result;
}

[[nodiscard]] inline TemporalJitter temporal_jitter(std::uint64_t frame_index,
                                                    rhi::Extent2D extent) noexcept {
    if (extent.width == 0U || extent.height == 0U)
        return {};
    const auto sample = static_cast<std::uint32_t>(frame_index % 8U) + 1U;
    const float width = static_cast<float>(extent.width);
    const float height = static_cast<float>(extent.height);
    return {
        (halton(sample, 2U) - 0.5F) * 2.0F / width,
        (halton(sample, 3U) - 0.5F) * 2.0F / height,
    };
}

// Applies an NDC-space offset to clip-space x/y without changing depth. This
// is J * projection, where J adds jitter * clip.w to the first two clip rows.
[[nodiscard]] inline rhi::Matrix4 jitter_projection(rhi::Matrix4 projection,
                                                    TemporalJitter jitter) noexcept {
    for (std::size_t column = 0; column < 4; ++column) {
        projection.values[column * 4] += jitter.x * projection.values[column * 4 + 3];
        projection.values[column * 4 + 1] += jitter.y * projection.values[column * 4 + 3];
    }
    return projection;
}

} // namespace astra::renderer

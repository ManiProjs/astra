#pragma once

#include <astra/world/world_streamer.hpp>

#include <cstddef>

namespace astra::world {

inline constexpr std::size_t terrain_lod_count = 3;

[[nodiscard]] constexpr std::size_t terrain_lod_index(TerrainLod lod) noexcept {
    return static_cast<std::size_t>(lod);
}

struct VegetationLodDensity final {
    unsigned trees{};
    unsigned grass_clumps{};
    unsigned rocks{};
    unsigned shrubs{};
};

[[nodiscard]] constexpr VegetationLodDensity vegetation_lod_density(TerrainLod lod) noexcept {
    switch (lod) {
    case TerrainLod::near:
        return {5U, 720U, 2U, 4U};
    case TerrainLod::medium:
        return {3U, 240U, 1U, 2U};
    case TerrainLod::far:
        return {1U, 0U, 0U, 0U};
    }
    return {};
}

} // namespace astra::world

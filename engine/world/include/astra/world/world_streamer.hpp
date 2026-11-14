#pragma once

#include <astra/core/thread_pool.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <future>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace astra::world {

struct ChunkCoord {
    std::int32_t x{};
    std::int32_t z{};
    friend auto operator<=>(const ChunkCoord&, const ChunkCoord&) = default;
};

enum class ChunkState : std::uint8_t { queued, ready };
enum class TerrainLod : std::uint8_t { near, medium, far };

struct TerrainChunk {
    ChunkCoord coord{};
    float world_origin_x{};
    float world_origin_z{};
    float size{};
    std::uint32_t resolution{};
    // Row-major (z * resolution + x), including both edges.
    std::vector<float> heights;

    [[nodiscard]] float height_at(std::uint32_t x, std::uint32_t z) const {
        if (x > resolution || z > resolution || heights.size() !=
                static_cast<std::size_t>(resolution + 1U) * (resolution + 1U)) {
            throw std::out_of_range("terrain sample is outside the chunk");
        }
        return heights[static_cast<std::size_t>(z) * (resolution + 1U) + x];
    }
};

struct ChunkInfo {
    ChunkCoord coord{};
    ChunkState state{ChunkState::queued};
    TerrainLod lod{TerrainLod::near};
    float distance{};
};

struct WorldStreamerConfig {
    float chunk_size{64.0F};
    std::uint32_t resolution{32};
    std::int32_t load_radius{3};
    std::int32_t unload_radius{4};
    std::size_t max_in_flight{4};
    std::size_t max_resident_chunks{256};
    float medium_lod_distance{110.0F};
    float far_lod_distance{190.0F};
};

// Main-thread-owned streaming coordinator. Generation runs on the supplied pool;
// max_in_flight bounds queued + running jobs even though ThreadPool itself is unbounded.
class WorldStreamer final {
  public:
    using HeightFunction = std::function<float(float, float)>;

    explicit WorldStreamer(core::ThreadPool& pool, WorldStreamerConfig config = {},
                           HeightFunction height_function = default_height)
        : pool_(pool), config_(config), height_function_(std::move(height_function)) {
        if (!std::isfinite(config_.chunk_size) || config_.chunk_size <= 0.0F ||
            config_.chunk_size > 1.0e6F || config_.resolution == 0 ||
            config_.resolution > 256 || config_.load_radius < 0 ||
            config_.unload_radius < config_.load_radius || config_.max_in_flight == 0 ||
            config_.max_in_flight > 64 || config_.max_resident_chunks == 0 ||
            config_.max_resident_chunks > 4096 ||
            (static_cast<std::uint64_t>(config_.unload_radius) * 2U + 1U) *
                    (static_cast<std::uint64_t>(config_.unload_radius) * 2U + 1U) >
                config_.max_resident_chunks ||
            !std::isfinite(config_.medium_lod_distance) ||
            !std::isfinite(config_.far_lod_distance) ||
            config_.medium_lod_distance < 0.0F ||
            config_.far_lod_distance < config_.medium_lod_distance || !height_function_) {
            throw std::invalid_argument("invalid WorldStreamer configuration");
        }
    }

    WorldStreamer(const WorldStreamer&) = delete;
    WorldStreamer& operator=(const WorldStreamer&) = delete;

    // Call once per frame (or at a lower cadence). Ready futures are harvested first,
    // then missing chunks are queued nearest-first up to max_in_flight.
    void update(float world_x, float world_z) {
        if (!std::isfinite(world_x) || !std::isfinite(world_z))
            throw std::invalid_argument("world position must be finite");
        const ChunkCoord next_center{world_to_chunk(world_x), world_to_chunk(world_z)};
        const bool center_changed = !initialized_ || next_center != center_;
        center_ = next_center;
        initialized_ = true;
        const bool harvested = harvest();
        if (center_changed)
            evict_distant();
        if (center_changed || harvested)
            queue_missing();
        if (center_changed || harvested)
            refresh_metadata(world_x, world_z);
        else
            update_metadata_distances(world_x, world_z);
    }

    // Also useful when the camera is stationary and the caller wants to harvest jobs.
    void pump(float world_x, float world_z) { update(world_x, world_z); }

    [[nodiscard]] const std::map<ChunkCoord, TerrainChunk>& chunks() const noexcept {
        return chunks_;
    }
    [[nodiscard]] const std::map<ChunkCoord, ChunkInfo>& metadata() const noexcept {
        return metadata_;
    }
    [[nodiscard]] std::size_t in_flight() const noexcept { return pending_.size(); }
    [[nodiscard]] ChunkCoord center_chunk() const noexcept { return center_; }

    [[nodiscard]] static float default_height(float x, float z) noexcept {
        const float broad = std::sin(x * 0.027F) * std::cos(z * 0.023F) * 3.2F;
        const float detail = std::sin((x + z) * 0.075F) * 0.7F +
                             std::cos((x - z) * 0.061F) * 0.55F;
        const float hill = 5.0F * std::exp(-((x - 43.0F) * (x - 43.0F) +
                                            (z + 48.0F) * (z + 48.0F)) / 1500.0F);
        return broad + detail + hill;
    }

  private:
    struct PendingChunk {
        std::future<TerrainChunk> future;
    };

    [[nodiscard]] std::int32_t world_to_chunk(float value) const {
        const double coordinate = std::floor(static_cast<double>(value) / config_.chunk_size);
        if (coordinate < std::numeric_limits<std::int32_t>::min() ||
            coordinate > std::numeric_limits<std::int32_t>::max())
            throw std::out_of_range("world position exceeds chunk coordinate range");
        return static_cast<std::int32_t>(coordinate);
    }

    [[nodiscard]] bool harvest() {
        bool changed = false;
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            try {
                auto chunk = it->second.future.get();
                const auto dx = std::abs(static_cast<std::int64_t>(it->first.x) - center_.x);
                const auto dz = std::abs(static_cast<std::int64_t>(it->first.z) - center_.z);
                if (dx <= config_.unload_radius && dz <= config_.unload_radius)
                    chunks_.insert_or_assign(it->first, std::move(chunk));
            } catch (...) {
                // Failed generation is retried by the next update while in range.
            }
            it = pending_.erase(it);
            changed = true;
        }
        return changed;
    }

    void evict_distant() {
        const auto radius = config_.unload_radius;
        for (auto it = chunks_.begin(); it != chunks_.end();) {
            if (std::abs(static_cast<std::int64_t>(it->first.x) - center_.x) > radius ||
                std::abs(static_cast<std::int64_t>(it->first.z) - center_.z) > radius)
                it = chunks_.erase(it);
            else
                ++it;
        }
        // A submitted job cannot be cancelled in the current ThreadPool. Its result is
        // discarded on completion when it has moved outside the unload radius.
    }

    void queue_missing() {
        std::vector<ChunkCoord> candidates;
        const auto radius = config_.load_radius;
        for (std::int32_t z = -radius; z <= radius; ++z) {
            for (std::int32_t x = -radius; x <= radius; ++x) {
                const auto chunk_x = static_cast<std::int64_t>(center_.x) + x;
                const auto chunk_z = static_cast<std::int64_t>(center_.z) + z;
                if (chunk_x < std::numeric_limits<std::int32_t>::min() ||
                    chunk_x > std::numeric_limits<std::int32_t>::max() ||
                    chunk_z < std::numeric_limits<std::int32_t>::min() ||
                    chunk_z > std::numeric_limits<std::int32_t>::max())
                    continue;
                const ChunkCoord coord{static_cast<std::int32_t>(chunk_x),
                                       static_cast<std::int32_t>(chunk_z)};
                if (!chunks_.contains(coord) && !pending_.contains(coord))
                    candidates.push_back(coord);
            }
        }
        std::sort(candidates.begin(), candidates.end(), [this](ChunkCoord a, ChunkCoord b) {
            const auto distance_squared = [this](ChunkCoord c) {
                const auto dx = static_cast<std::int64_t>(c.x) - center_.x;
                const auto dz = static_cast<std::int64_t>(c.z) - center_.z;
                return dx * dx + dz * dz;
            };
            return distance_squared(a) < distance_squared(b);
        });
        for (const auto coord : candidates) {
            if (pending_.size() >= config_.max_in_flight)
                break;
            const auto function = height_function_;
            const auto config = config_;
            auto future = pool_.submit([coord, function, config] {
                TerrainChunk chunk;
                chunk.coord = coord;
                chunk.world_origin_x = static_cast<float>(coord.x) * config.chunk_size;
                chunk.world_origin_z = static_cast<float>(coord.z) * config.chunk_size;
                chunk.size = config.chunk_size;
                chunk.resolution = config.resolution;
                const auto side = static_cast<std::size_t>(config.resolution) + 1U;
                chunk.heights.resize(side * side);
                for (std::uint32_t z = 0; z <= config.resolution; ++z) {
                    for (std::uint32_t x = 0; x <= config.resolution; ++x) {
                        const float wx = chunk.world_origin_x + config.chunk_size *
                            static_cast<float>(x) / config.resolution;
                        const float wz = chunk.world_origin_z + config.chunk_size *
                            static_cast<float>(z) / config.resolution;
                        const float height = function(wx, wz);
                        if (!std::isfinite(height))
                            throw std::runtime_error("terrain height function returned a non-finite sample");
                        chunk.heights[static_cast<std::size_t>(z) * side + x] = height;
                    }
                }
                return chunk;
            });
            pending_.emplace(coord, PendingChunk{std::move(future)});
        }
    }

    void refresh_metadata(float world_x, float world_z) {
        metadata_.clear();
        const auto add = [this](ChunkCoord coord, ChunkState state) {
            metadata_.emplace(coord, ChunkInfo{coord, state, TerrainLod::near, 0.0F});
        };
        for (const auto& [coord, chunk] : chunks_) {
            (void)chunk;
            add(coord, ChunkState::ready);
        }
        for (const auto& [coord, pending] : pending_) {
            (void)pending;
            add(coord, ChunkState::queued);
        }
        update_metadata_distances(world_x, world_z);
    }

    void update_metadata_distances(float world_x, float world_z) {
        for (auto& [coord, info] : metadata_) {
            const float cx = (static_cast<float>(coord.x) + 0.5F) * config_.chunk_size;
            const float cz = (static_cast<float>(coord.z) + 0.5F) * config_.chunk_size;
            info.distance = std::hypot(cx - world_x, cz - world_z);
            info.lod = info.distance >= config_.far_lod_distance ? TerrainLod::far :
                info.distance >= config_.medium_lod_distance ? TerrainLod::medium : TerrainLod::near;
        }
    }

    core::ThreadPool& pool_;
    WorldStreamerConfig config_;
    HeightFunction height_function_;
    ChunkCoord center_{};
    bool initialized_{};
    std::map<ChunkCoord, TerrainChunk> chunks_;
    std::map<ChunkCoord, PendingChunk> pending_;
    std::map<ChunkCoord, ChunkInfo> metadata_;
};

} // namespace astra::world

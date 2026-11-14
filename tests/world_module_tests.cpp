#include <astra/core/thread_pool.hpp>
#include <astra/world/character_controller.hpp>
#include <astra/world/vegetation_lod.hpp>
#include <astra/world/world_streamer.hpp>

#include <cassert>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

int main() {
    using namespace astra::world;

    {
        static_assert(terrain_lod_count == 3);
        static_assert(terrain_lod_index(TerrainLod::near) == 0);
        static_assert(terrain_lod_index(TerrainLod::medium) == 1);
        static_assert(terrain_lod_index(TerrainLod::far) == 2);
        const auto near = vegetation_lod_density(TerrainLod::near);
        const auto medium = vegetation_lod_density(TerrainLod::medium);
        const auto far = vegetation_lod_density(TerrainLod::far);
        assert(near.trees > medium.trees && medium.trees > far.trees);
        assert(near.grass_clumps > medium.grass_clumps && medium.grass_clumps > far.grass_clumps);
        assert(near.rocks > medium.rocks && medium.rocks > far.rocks);
        assert(near.shrubs > medium.shrubs && medium.shrubs > far.shrubs);
    }

    {
        astra::core::ThreadPool pool{2};
        WorldStreamerConfig config;
        config.chunk_size = 16.0F;
        config.resolution = 2;
        config.load_radius = 0;
        config.unload_radius = 0;
        config.max_in_flight = 1;
        config.medium_lod_distance = 30.0F;
        config.far_lod_distance = 60.0F;
        WorldStreamer streamer{pool, config, [](float x, float z) { return x + z; }};

        streamer.update(-0.1F, -0.1F);
        assert(streamer.center_chunk() == (ChunkCoord{-1, -1}));
        assert(streamer.in_flight() == 1);
        assert(streamer.metadata().at({-1, -1}).state == ChunkState::queued);
        for (int attempt = 0; attempt < 1000 && streamer.chunks().empty(); ++attempt) {
            streamer.update(-0.1F, -0.1F);
            std::this_thread::yield();
        }
        assert(streamer.chunks().size() == 1);
        const auto& first = streamer.chunks().at({-1, -1});
        assert(first.heights.size() == 9);
        assert(std::abs(first.height_at(0, 0) - (-32.0F)) < 0.0001F);
        assert(std::abs(first.height_at(2, 2) - 0.0F) < 0.0001F);
        assert(streamer.metadata().at({-1, -1}).state == ChunkState::ready);

        streamer.update(16.1F, 0.1F);
        assert(streamer.center_chunk() == (ChunkCoord{1, 0}));
        assert(streamer.chunks().empty());
        assert(streamer.in_flight() <= 1);
        pool.shutdown();
    }

    {
        // Stationary updates must harvest futures without rebuilding the metadata map.
        astra::core::ThreadPool pool{1};
        WorldStreamerConfig config;
        config.resolution = 1;
        config.load_radius = 0;
        config.unload_radius = 0;
        config.max_in_flight = 1;
        config.medium_lod_distance = 40.0F;
        config.far_lod_distance = 80.0F;
        std::atomic_bool release_generation{false};
        WorldStreamer streamer{pool, config, [&release_generation](float, float) {
                                   while (!release_generation.load(std::memory_order_acquire))
                                       std::this_thread::yield();
                                   return 0.0F;
                               }};
        streamer.update(0.0F, 0.0F);
        const auto* metadata_entry = &streamer.metadata().at({0, 0});
        assert(metadata_entry->lod == TerrainLod::medium);
        streamer.update(0.0F, 0.0F);
        assert(&streamer.metadata().at({0, 0}) == metadata_entry);
        streamer.update(32.0F, 32.0F);
        assert(&streamer.metadata().at({0, 0}) == metadata_entry);
        assert(metadata_entry->lod == TerrainLod::near);
        release_generation.store(true, std::memory_order_release);
        pool.shutdown();
        streamer.update(0.0F, 0.0F);
        assert(streamer.chunks().contains({0, 0}));
        assert(streamer.metadata().at({0, 0}).state == ChunkState::ready);
    }

    {
        astra::core::ThreadPool pool{1};
        WorldStreamerConfig config;
        config.resolution = 1;
        config.load_radius = 3;
        config.unload_radius = 3;
        config.max_in_flight = 2;
        WorldStreamer streamer{pool, config, [](float x, float z) {
                                   std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                   return x - z;
                               }};
        streamer.update(0.0F, 0.0F);
        assert(streamer.in_flight() == 2);
        pool.shutdown();
    }

    {
        astra::core::ThreadPool pool{1};
        WorldStreamerConfig invalid;
        invalid.resolution = 257;
        bool rejected_resolution = false;
        try {
            WorldStreamer streamer{pool, invalid};
            (void)streamer;
        } catch (const std::invalid_argument&) {
            rejected_resolution = true;
        }
        assert(rejected_resolution);

        invalid = WorldStreamerConfig{};
        invalid.unload_radius = 9;
        bool rejected_resident_budget = false;
        try {
            WorldStreamer streamer{pool, invalid};
            (void)streamer;
        } catch (const std::invalid_argument&) {
            rejected_resident_budget = true;
        }
        assert(rejected_resident_budget);
        pool.shutdown();
    }

    {
        CharacterController controller;
        const auto flat = [](float, float) { return 0.0F; };
        controller.set_position({0.0F, 0.0F, 0.0F});
        controller.step(1.0F / 60.0F, {}, flat);
        assert(controller.state().grounded);
        controller.step(1.0F / 60.0F, {.jump_pressed = true}, flat);
        assert(!controller.state().grounded);
        assert(controller.state().velocity.y > 0.0F);
        for (int frame = 0; frame < 180; ++frame)
            controller.step(1.0F / 60.0F, {}, flat);
        assert(controller.state().grounded);
        assert(std::abs(controller.state().position.y) < 0.0001F);

        const float before = controller.state().position.x;
        controller.step(0.1F, {.move_x = 1.0F}, flat);
        assert(controller.state().position.x > before);
    }

    {
        CharacterController controller;
        const auto steep = [](float x, float) { return x > 0.0F ? x * 2.0F : 0.0F; };
        controller.set_position({0.0F, 0.0F, 0.0F});
        controller.step(1.0F / 60.0F, {}, steep);
        const float before = controller.state().position.x;
        controller.step(0.1F, {.move_x = 1.0F}, steep);
        assert(controller.state().position.x <= before + 0.0001F);
    }

    {
        CharacterController controller;
        const auto diagonal_ridge = [](float x, float z) {
            return x > 0.2F && z > 0.2F ? 0.1F : 0.0F;
        };
        controller.set_position({0.0F, 0.0F, 0.0F});
        controller.step(1.0F / 60.0F, {}, diagonal_ridge);
        // The ridge intersects the capsule at the +X/+Z footprint sample;
        // a center-plus-cardinal sampler incorrectly leaves the feet at y=0.
        assert(std::abs(controller.state().position.y - 0.1F) < 0.0001F);
    }

    {
        CharacterController controller;
        bool rejected_non_finite_height = false;
        try {
            controller.step(1.0F / 60.0F, {}, [](float x, float) {
                return x > 0.1F ? std::numeric_limits<float>::quiet_NaN() : 0.0F;
            });
        } catch (const std::runtime_error&) {
            rejected_non_finite_height = true;
        }
        assert(rejected_non_finite_height);
    }
    return 0;
}

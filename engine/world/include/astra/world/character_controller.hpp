#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>

namespace astra::world {

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

struct CharacterControllerConfig {
    float radius{0.38F};
    float height{1.8F};
    float gravity{24.0F};
    float jump_speed{8.0F};
    float max_slope_degrees{50.0F};
    float max_step_height{0.45F};
    float ground_snap_distance{0.12F};
    float skin{0.025F};
    float max_horizontal_speed{7.0F};
};

struct CharacterInput {
    float move_x{};
    float move_z{};
    float speed_scale{1.0F};
    bool jump_pressed{};
};

struct CharacterState {
    // Position is the center of the capsule's bottom hemisphere (feet level).
    Vec3 position{};
    Vec3 velocity{};
    bool grounded{};
};

// Heightfield capsule controller. The supplied sampler returns world-space terrain
// height for any finite X/Z. The capsule is approximated by five footprint samples.
class CharacterController final {
  public:
    using HeightSampler = std::function<float(float, float)>;

    explicit CharacterController(CharacterControllerConfig config = {}) : config_(config) {
        if (!std::isfinite(config_.radius) || config_.radius <= 0.0F ||
            !std::isfinite(config_.height) || config_.height < 2.0F * config_.radius ||
            !std::isfinite(config_.gravity) || config_.gravity < 0.0F ||
            !std::isfinite(config_.jump_speed) || config_.jump_speed < 0.0F ||
            !std::isfinite(config_.max_slope_degrees) || config_.max_slope_degrees < 0.0F ||
            config_.max_slope_degrees >= 90.0F ||
            !std::isfinite(config_.max_step_height) || config_.max_step_height < 0.0F ||
            !std::isfinite(config_.ground_snap_distance) || config_.ground_snap_distance < 0.0F ||
            !std::isfinite(config_.skin) || config_.skin < 0.0F ||
            !std::isfinite(config_.max_horizontal_speed) || config_.max_horizontal_speed < 0.0F) {
            throw std::invalid_argument("invalid CharacterController configuration");
        }
    }

    [[nodiscard]] const CharacterState& state() const noexcept { return state_; }
    [[nodiscard]] const CharacterControllerConfig& config() const noexcept { return config_; }
    void set_position(Vec3 position) {
        if (!finite(position))
            throw std::invalid_argument("character position must be finite");
        state_.position = position;
        state_.velocity = {};
        state_.grounded = false;
    }

    // Delta time is capped at 0.25 s and integrated in <= 1/60 s substeps to keep
    // terrain contact stable after frame hitches. Input is a desired horizontal direction.
    void step(float delta_time, CharacterInput input, const HeightSampler& height_at) {
        if (!std::isfinite(delta_time) || delta_time < 0.0F || !finite_input(input) || !height_at)
            throw std::invalid_argument("invalid character step arguments");
        const float duration = std::min(delta_time, 0.25F);
        if (duration == 0.0F)
            return;

        float move_length = std::hypot(input.move_x, input.move_z);
        if (move_length > 1.0F) {
            input.move_x /= move_length;
            input.move_z /= move_length;
        }
        const float horizontal_speed = config_.max_horizontal_speed *
                                       std::clamp(input.speed_scale, 0.0F, 2.0F);
        state_.velocity.x = input.move_x * horizontal_speed;
        state_.velocity.z = input.move_z * horizontal_speed;

        const int steps = std::max(1, static_cast<int>(std::ceil(duration * 60.0F)));
        const float dt = duration / static_cast<float>(steps);
        for (int index = 0; index < steps; ++index) {
            integrate_horizontal(dt, height_at);
            integrate_vertical(dt, index == 0 && input.jump_pressed, height_at);
        }
    }

  private:
    [[nodiscard]] static bool finite(Vec3 value) noexcept {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }
    [[nodiscard]] static bool finite_input(const CharacterInput& input) noexcept {
        return std::isfinite(input.move_x) && std::isfinite(input.move_z) &&
               std::isfinite(input.speed_scale);
    }

    [[nodiscard]] float support_height(float x, float z, const HeightSampler& height_at) const {
        // Sample the full capsule footprint: cardinal-only samples miss terrain
        // ridges that intersect the capsule near its diagonal quadrants.
        constexpr float diagonal = 0.70710678118F;
        const std::array<std::array<float, 2>, 9> offsets{{
            {{0.0F, 0.0F}},
            {{config_.radius, 0.0F}},
            {{-config_.radius, 0.0F}},
            {{0.0F, config_.radius}},
            {{0.0F, -config_.radius}},
            {{config_.radius * diagonal, config_.radius * diagonal}},
            {{-config_.radius * diagonal, config_.radius * diagonal}},
            {{config_.radius * diagonal, -config_.radius * diagonal}},
            {{-config_.radius * diagonal, -config_.radius * diagonal}},
        }};
        float maximum = -std::numeric_limits<float>::infinity();
        for (const auto& offset : offsets) {
            const float sample = height_at(x + offset[0], z + offset[1]);
            if (!std::isfinite(sample))
                throw std::runtime_error("heightfield returned a non-finite height");
            maximum = std::max(maximum, sample);
        }
        return maximum;
    }

    [[nodiscard]] bool walkable(float x, float z, float height, const HeightSampler& sampler) const {
        const auto sample = [&](float sample_x, float sample_z) {
            const float value = sampler(sample_x, sample_z);
            if (!std::isfinite(value))
                throw std::runtime_error("heightfield returned a non-finite height");
            return value;
        };
        const float center = sample(x, z);
        const float dx = (sample(x + config_.radius, z) -
                          sample(x - config_.radius, z)) / (2.0F * config_.radius);
        const float dz = (sample(x, z + config_.radius) -
                          sample(x, z - config_.radius)) / (2.0F * config_.radius);
        if (!std::isfinite(dx) || !std::isfinite(dz))
            throw std::runtime_error("heightfield returned a non-finite slope");
        const float slope = std::atan(std::hypot(dx, dz)) * 57.2957795F;
        return slope <= config_.max_slope_degrees && height - center <= config_.max_step_height;
    }

    void integrate_horizontal(float dt, const HeightSampler& height_at) {
        const float next_x = state_.position.x + state_.velocity.x * dt;
        const float next_z = state_.position.z + state_.velocity.z * dt;
        const float old_support = support_height(state_.position.x, state_.position.z, height_at);
        const float next_support = support_height(next_x, next_z, height_at);
        if (walkable(next_x, next_z, next_support, height_at) &&
            next_support <= old_support + config_.max_step_height + config_.skin) {
            state_.position.x = next_x;
            state_.position.z = next_z;
        } else {
            // Resolve axes independently so the player can slide along a steep face.
            const float x_support = support_height(next_x, state_.position.z, height_at);
            if (walkable(next_x, state_.position.z, x_support, height_at) &&
                x_support <= old_support + config_.max_step_height + config_.skin)
                state_.position.x = next_x;
            else
                state_.velocity.x = 0.0F;
            const float z_support = support_height(state_.position.x, next_z, height_at);
            if (walkable(state_.position.x, next_z, z_support, height_at) &&
                z_support <= old_support + config_.max_step_height + config_.skin)
                state_.position.z = next_z;
            else
                state_.velocity.z = 0.0F;
        }
    }

    void integrate_vertical(float dt, bool jump_pressed, const HeightSampler& height_at) {
        const float ground = support_height(state_.position.x, state_.position.z, height_at);
        const float feet = state_.position.y;
        if (state_.grounded && jump_pressed) {
            state_.velocity.y = config_.jump_speed;
            state_.grounded = false;
        }
        state_.velocity.y -= config_.gravity * dt;
        state_.position.y += state_.velocity.y * dt;

        const float penetration = ground - state_.position.y;
        if (state_.velocity.y <= 0.0F && penetration >= -config_.skin &&
            walkable(state_.position.x, state_.position.z, ground, height_at)) {
            state_.position.y = ground;
            state_.velocity.y = 0.0F;
            state_.grounded = true;
        } else if (state_.velocity.y > 0.0F) {
            state_.grounded = false;
        } else if (feet - ground <= config_.ground_snap_distance &&
                   walkable(state_.position.x, state_.position.z, ground, height_at)) {
            state_.position.y = ground;
            state_.velocity.y = 0.0F;
            state_.grounded = true;
        } else {
            state_.grounded = false;
        }
    }

    CharacterControllerConfig config_;
    CharacterState state_{};
};

} // namespace astra::world

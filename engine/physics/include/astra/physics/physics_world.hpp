#pragma once

#include <astra/rhi/types.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace astra::physics {

struct Quaternion final {
    float x{};
    float y{};
    float z{};
    float w{1.0F};
};

enum class MotionType : unsigned char { static_body, kinematic, dynamic };
enum class ShapeType : unsigned char { sphere, box, capsule, cylinder };

struct Shape final {
    ShapeType type{ShapeType::box};
    // Sphere: radius in x. Box: half extents. Capsule/cylinder: radius in x,
    // half-height in y. All dimensions are in world units and must be positive.
    rhi::Vec3 dimensions{0.5F, 0.5F, 0.5F};
};

struct BodyDesc final {
    Shape shape{};
    rhi::Vec3 position{};
    Quaternion rotation{};
    rhi::Vec3 linear_velocity{};
    MotionType motion{MotionType::dynamic};
    float mass{1.0F};
    float friction{0.2F};
    float restitution{};
    float linear_damping{0.05F};
    float angular_damping{0.05F};
    bool sensor{};
};

struct BodyHandle final {
    std::uint32_t value{};
    friend bool operator==(BodyHandle, BodyHandle) = default;
};

enum class ConstraintType : unsigned char { fixed, hinge, distance };

struct ConstraintHandle final {
    std::uint32_t value{};
    friend bool operator==(ConstraintHandle, ConstraintHandle) = default;
};

struct ConstraintDesc final {
    ConstraintType type{ConstraintType::fixed};
    BodyHandle body_a{};
    BodyHandle body_b{};
    // Anchors and axes are local to each body's center of mass.
    rhi::Vec3 local_anchor_a{};
    rhi::Vec3 local_anchor_b{};
    rhi::Vec3 local_axis_a{0.0F, 1.0F, 0.0F};
    rhi::Vec3 local_axis_b{0.0F, 1.0F, 0.0F};
    rhi::Vec3 local_normal_a{1.0F, 0.0F, 0.0F};
    rhi::Vec3 local_normal_b{1.0F, 0.0F, 0.0F};
    float min_limit{}; // Hinge angle in radians or distance in world units.
    float max_limit{};
};

struct ContactEvent final {
    BodyHandle body_a{};
    BodyHandle body_b{};
    rhi::Vec3 position{};
    rhi::Vec3 normal{}; // Points from body_a toward body_b.
    float penetration_depth{};
};

struct BodyState final {
    rhi::Vec3 position{};
    Quaternion rotation{};
    rhi::Vec3 linear_velocity{};
};

struct CharacterHandle final {
    std::uint32_t value{};
    friend bool operator==(CharacterHandle, CharacterHandle) = default;
};

struct CharacterDesc final {
    // Half length of the capsule's cylindrical section and its spherical radius.
    // The character origin is at the bottom of the capsule.
    float capsule_half_height{0.675F};
    float capsule_radius{0.3F};
    rhi::Vec3 position{};
    Quaternion rotation{};
    float mass{70.0F};
    float max_strength{100.0F};
    float max_slope_angle_radians{0.87266463F}; // 50 degrees
};

enum class CharacterGroundState : unsigned char {
    on_ground,
    on_steep_ground,
    not_supported,
    in_air,
};

struct CharacterState final {
    rhi::Vec3 position{};
    rhi::Vec3 linear_velocity{};
    CharacterGroundState ground_state{CharacterGroundState::in_air};
};

struct RayHit final {
    BodyHandle body{};
    rhi::Vec3 position{};
    float distance{};
};

struct PhysicsConfig final {
    rhi::Vec3 gravity{0.0F, -9.81F, 0.0F};
    float fixed_step_seconds{1.0F / 60.0F};
    std::uint32_t max_substeps{4};
    std::uint32_t max_bodies{4096};
    std::uint32_t max_body_pairs{16384};
    std::uint32_t max_contact_constraints{8192};
    std::uint32_t max_contact_events{16384};
    std::uint32_t worker_threads{}; // 0 chooses a conservative hardware-based default.
};

struct PhysicsError final {
    std::string message;
};

// Fixed-step 3D rigid-body simulation backed by Jolt Physics. Bodies and shape
// ownership stay inside the world; handles remain valid until destroy_body().
class PhysicsWorld final {
  public:
    [[nodiscard]] static std::expected<PhysicsWorld, PhysicsError> create(PhysicsConfig config = {});
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) noexcept;
    PhysicsWorld& operator=(PhysicsWorld&&) noexcept;

    [[nodiscard]] std::expected<BodyHandle, PhysicsError> create_body(const BodyDesc& desc);
    // Creates a static triangle-mesh collider. Indices are grouped as
    // counter-clockwise triangles; suitable for terrain and static level geo.
    [[nodiscard]] std::expected<BodyHandle, PhysicsError>
    create_static_mesh(std::span<const rhi::Vec3> vertices,
                       std::span<const std::uint32_t> triangle_indices,
                       rhi::Vec3 position = {});
    [[nodiscard]] bool destroy_body(BodyHandle body) noexcept;
    [[nodiscard]] std::expected<ConstraintHandle, PhysicsError>
    create_constraint(const ConstraintDesc& desc);
    [[nodiscard]] bool destroy_constraint(ConstraintHandle constraint) noexcept;
    // Thread-safe; returns and clears queued contact events. The queue is bounded by
    // PhysicsConfig::max_contact_events; newest events are dropped while it is full.
    [[nodiscard]] std::vector<ContactEvent> drain_contact_events();
    [[nodiscard]] std::expected<CharacterHandle, PhysicsError>
    create_character(const CharacterDesc& desc);
    [[nodiscard]] bool destroy_character(CharacterHandle character) noexcept;
    // Horizontal velocity is expressed in world space; the vertical component is ignored.
    [[nodiscard]] bool set_horizontal_velocity(CharacterHandle character,
                                               rhi::Vec3 velocity) noexcept;
    // Queues a jump to be applied when the character is supported by walkable ground.
    [[nodiscard]] bool jump(CharacterHandle character, float speed) noexcept;
    [[nodiscard]] std::expected<CharacterState, PhysicsError>
    character_state(CharacterHandle character) const;
    [[nodiscard]] std::expected<void, PhysicsError> step(float delta_time);
    [[nodiscard]] std::expected<BodyState, PhysicsError> body_state(BodyHandle body) const;
    [[nodiscard]] std::expected<std::optional<RayHit>, PhysicsError>
    raycast(rhi::Vec3 origin, rhi::Vec3 direction, float max_distance) const;
    [[nodiscard]] bool set_linear_velocity(BodyHandle body, rhi::Vec3 velocity) noexcept;
    [[nodiscard]] bool apply_impulse(BodyHandle body, rhi::Vec3 impulse) noexcept;
    [[nodiscard]] float interpolation_alpha() const noexcept;
    [[nodiscard]] std::uint32_t body_count() const noexcept;

  private:
    struct Impl;
    explicit PhysicsWorld(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace astra::physics

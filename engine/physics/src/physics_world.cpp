#include <astra/physics/physics_world.hpp>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace astra::physics {
namespace {

constexpr JPH::ObjectLayer kStaticLayer = 0;
constexpr JPH::ObjectLayer kMovingLayer = 1;

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
  public:
    [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return JPH::BroadPhaseLayer{static_cast<JPH::BroadPhaseLayer::Type>(layer)};
    }
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
  public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer,
                                     JPH::BroadPhaseLayer broad_phase) const override {
        return layer == kMovingLayer ||
               static_cast<JPH::BroadPhaseLayer::Type>(broad_phase) == kMovingLayer;
    }
};

class ObjectLayerPairs final : public JPH::ObjectLayerPairFilter {
  public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a == kMovingLayer || b == kMovingLayer;
    }
};

void initialize_jolt() {
    static std::once_flag once;
    std::call_once(once, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

[[nodiscard]] bool finite(rhi::Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] bool finite(Quaternion value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
           std::isfinite(value.w);
}

[[nodiscard]] float length_squared(rhi::Vec3 value) noexcept {
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

[[nodiscard]] float dot(rhi::Vec3 a, rhi::Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] rhi::Vec3 normalized(rhi::Vec3 value) noexcept {
    const float inverse_length = 1.0F / std::sqrt(length_squared(value));
    return {value.x * inverse_length, value.y * inverse_length, value.z * inverse_length};
}

class ContactCollector final : public JPH::ContactListener {
  public:
    explicit ContactCollector(std::size_t maximum_events) : maximum_events_(maximum_events) {}

    void OnContactAdded(const JPH::Body& body_a, const JPH::Body& body_b,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings&) override {
        collect(body_a, body_b, manifold);
    }

    void OnContactPersisted(const JPH::Body& body_a, const JPH::Body& body_b,
                            const JPH::ContactManifold& manifold,
                            JPH::ContactSettings&) override {
        collect(body_a, body_b, manifold);
    }

    std::vector<ContactEvent> drain() {
        std::lock_guard lock(mutex_);
        std::vector<ContactEvent> result;
        result.swap(events_);
        std::erase_if(result, [this](const ContactEvent& event) {
            return !active_bodies_.contains(event.body_a.value) ||
                   !active_bodies_.contains(event.body_b.value);
        });
        return result;
    }

    void add_body(BodyHandle body) {
        std::lock_guard lock(mutex_);
        active_bodies_.insert(body.value);
    }

    void remove_body(BodyHandle body) {
        std::lock_guard lock(mutex_);
        active_bodies_.erase(body.value);
        std::erase_if(events_, [body](const ContactEvent& event) {
            return event.body_a == body || event.body_b == body;
        });
    }

  private:
    void collect(const JPH::Body& body_a, const JPH::Body& body_b,
                 const JPH::ContactManifold& manifold) {
        const auto handle_a = static_cast<std::uint32_t>(body_a.GetUserData());
        const auto handle_b = static_cast<std::uint32_t>(body_b.GetUserData());
        if (handle_a == 0 || handle_b == 0 || manifold.mRelativeContactPointsOn1.empty())
            return;
        const auto point_a = manifold.GetWorldSpaceContactPointOn1(0);
        const auto point_b = manifold.GetWorldSpaceContactPointOn2(0);
        const auto midpoint = (point_a + point_b) * 0.5;
        const auto& normal = manifold.mWorldSpaceNormal;
        ContactEvent event{
            .body_a = BodyHandle{handle_a},
            .body_b = BodyHandle{handle_b},
            .position = {static_cast<float>(midpoint.GetX()), static_cast<float>(midpoint.GetY()),
                         static_cast<float>(midpoint.GetZ())},
            .normal = {normal.GetX(), normal.GetY(), normal.GetZ()},
            .penetration_depth = manifold.mPenetrationDepth,
        };
        std::lock_guard lock(mutex_);
        if (events_.size() < maximum_events_ && active_bodies_.contains(handle_a) &&
            active_bodies_.contains(handle_b))
            events_.push_back(event);
    }

    std::mutex mutex_;
    std::vector<ContactEvent> events_;
    std::unordered_set<std::uint32_t> active_bodies_;
    const std::size_t maximum_events_;
};

[[nodiscard]] JPH::EMotionType to_jolt(MotionType type) noexcept {
    switch (type) {
    case MotionType::static_body: return JPH::EMotionType::Static;
    case MotionType::kinematic: return JPH::EMotionType::Kinematic;
    case MotionType::dynamic: return JPH::EMotionType::Dynamic;
    }
    return JPH::EMotionType::Dynamic;
}

[[nodiscard]] JPH::RefConst<JPH::Shape> make_shape(const Shape& shape) {
    switch (shape.type) {
    case ShapeType::sphere:
        return new JPH::SphereShape(shape.dimensions.x);
    case ShapeType::box:
        return new JPH::BoxShape({shape.dimensions.x, shape.dimensions.y, shape.dimensions.z});
    case ShapeType::capsule:
        return new JPH::CapsuleShape(shape.dimensions.y, shape.dimensions.x);
    case ShapeType::cylinder:
        return new JPH::CylinderShape(shape.dimensions.y, shape.dimensions.x, 0.02F);
    }
    return nullptr;
}

} // namespace

struct PhysicsWorld::Impl final {
    explicit Impl(PhysicsConfig value)
        : config(value), contact_listener(value.max_contact_events),
          jobs(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                              static_cast<int>(value.worker_threads == 0
                                                   ? std::min(4U, std::max(
                                                                      1U, std::thread::hardware_concurrency() > 1
                                                                              ? std::thread::hardware_concurrency() - 1
                                                                              : 1U))
                                                   : value.worker_threads)),
          temporary_allocator(8U * 1024U * 1024U) {
        system.Init(config.max_bodies, 0, config.max_body_pairs, config.max_contact_constraints,
                    broad_phase_layers, object_vs_broad_phase, object_layer_pairs);
        system.SetGravity({config.gravity.x, config.gravity.y, config.gravity.z});
        system.SetContactListener(&contact_listener);
    }

    PhysicsConfig config;
    BroadPhaseLayers broad_phase_layers;
    ObjectVsBroadPhase object_vs_broad_phase;
    ObjectLayerPairs object_layer_pairs;
    // Declared before PhysicsSystem so it outlives the system that references it.
    ContactCollector contact_listener;
    JPH::PhysicsSystem system;
    JPH::JobSystemThreadPool jobs;
    JPH::TempAllocatorImpl temporary_allocator;
    std::unordered_map<std::uint32_t, JPH::BodyID> bodies;
    struct ConstraintRecord final {
        JPH::Ref<JPH::TwoBodyConstraint> constraint;
        BodyHandle body_a{};
        BodyHandle body_b{};
    };
    std::unordered_map<std::uint32_t, ConstraintRecord> constraints;
    struct CharacterRecord final {
        JPH::Ref<JPH::CharacterVirtual> character;
        rhi::Vec3 horizontal_velocity{};
        float jump_speed{};
        bool jump_requested{};
    };
    std::unordered_map<std::uint32_t, CharacterRecord> characters;
    std::uint32_t next_handle{1};
    std::uint32_t next_character_handle{1};
    std::uint32_t next_constraint_handle{1};
    float accumulator{};
};

PhysicsWorld::PhysicsWorld(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PhysicsWorld::~PhysicsWorld() = default;
PhysicsWorld::PhysicsWorld(PhysicsWorld&&) noexcept = default;
PhysicsWorld& PhysicsWorld::operator=(PhysicsWorld&&) noexcept = default;

std::expected<PhysicsWorld, PhysicsError> PhysicsWorld::create(PhysicsConfig config) {
    if (!finite(config.gravity) || !std::isfinite(config.fixed_step_seconds) ||
        config.fixed_step_seconds <= 0.0F || config.fixed_step_seconds > 0.1F ||
        config.max_substeps == 0 || config.max_substeps > 16 || config.max_bodies == 0 ||
        config.max_body_pairs == 0 || config.max_contact_constraints == 0 ||
        config.max_contact_events == 0 || config.max_contact_events > 1'000'000 ||
        config.worker_threads > 64) {
        return std::unexpected(PhysicsError{"PhysicsWorld configuration is invalid."});
    }
    initialize_jolt();
    try {
        return PhysicsWorld{std::make_unique<Impl>(config)};
    } catch (const std::exception& error) {
        return std::unexpected(PhysicsError{"Could not initialize Jolt Physics: " +
                                            std::string{error.what()}});
    }
}

std::expected<BodyHandle, PhysicsError> PhysicsWorld::create_body(const BodyDesc& desc) {
    if (!impl_ || !finite(desc.position) || !finite(desc.rotation) ||
        !finite(desc.linear_velocity) || !std::isfinite(desc.mass) || desc.mass <= 0.0F ||
        !std::isfinite(desc.friction) || desc.friction < 0.0F ||
        !std::isfinite(desc.restitution) || desc.restitution < 0.0F ||
        !std::isfinite(desc.linear_damping) || desc.linear_damping < 0.0F ||
        !std::isfinite(desc.angular_damping) || desc.angular_damping < 0.0F ||
        !finite(desc.shape.dimensions) || desc.shape.dimensions.x <= 0.0F ||
        ((desc.shape.type == ShapeType::box) &&
         (desc.shape.dimensions.y <= 0.0F || desc.shape.dimensions.z <= 0.0F)) ||
        ((desc.shape.type == ShapeType::capsule || desc.shape.type == ShapeType::cylinder) &&
         desc.shape.dimensions.y <= 0.0F)) {
        return std::unexpected(PhysicsError{"Rigid body description contains invalid values."});
    }
    if (impl_->bodies.size() >= impl_->config.max_bodies)
        return std::unexpected(PhysicsError{"PhysicsWorld reached its configured body limit."});

    auto shape = make_shape(desc.shape);
    if (shape == nullptr)
        return std::unexpected(PhysicsError{"Jolt could not create the requested collision shape."});
    const auto motion = to_jolt(desc.motion);
    const auto layer = motion == JPH::EMotionType::Static ? kStaticLayer : kMovingLayer;
    JPH::BodyCreationSettings settings{
        shape.GetPtr(), JPH::RVec3{desc.position.x, desc.position.y, desc.position.z},
        JPH::Quat{desc.rotation.x, desc.rotation.y, desc.rotation.z, desc.rotation.w}, motion, layer};
    const auto handle = impl_->next_handle++;
    settings.mUserData = handle;
    settings.mLinearVelocity = {desc.linear_velocity.x, desc.linear_velocity.y, desc.linear_velocity.z};
    settings.mFriction = desc.friction;
    settings.mRestitution = desc.restitution;
    settings.mLinearDamping = desc.linear_damping;
    settings.mAngularDamping = desc.angular_damping;
    settings.mIsSensor = desc.sensor;
    if (motion == JPH::EMotionType::Dynamic) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = desc.mass;
    }
    auto& body_interface = impl_->system.GetBodyInterface();
    const auto id = body_interface.CreateAndAddBody(
        settings, motion == JPH::EMotionType::Static ? JPH::EActivation::DontActivate
                                                      : JPH::EActivation::Activate);
    if (id.IsInvalid())
        return std::unexpected(PhysicsError{"Jolt could not allocate a rigid body."});
    impl_->bodies.emplace(handle, id);
    impl_->contact_listener.add_body(BodyHandle{handle});
    return BodyHandle{handle};
}

std::expected<BodyHandle, PhysicsError>
PhysicsWorld::create_static_mesh(std::span<const rhi::Vec3> vertices,
                                 std::span<const std::uint32_t> triangle_indices,
                                 rhi::Vec3 position) {
    if (!impl_ || vertices.size() < 3 || triangle_indices.empty() ||
        triangle_indices.size() % 3 != 0 || !finite(position)) {
        return std::unexpected(PhysicsError{"Static mesh requires finite position, vertices and triangle indices."});
    }
    if (impl_->bodies.size() >= impl_->config.max_bodies)
        return std::unexpected(PhysicsError{"PhysicsWorld reached its configured body limit."});

    JPH::VertexList jolt_vertices;
    jolt_vertices.reserve(vertices.size());
    for (const auto vertex : vertices) {
        if (!finite(vertex))
            return std::unexpected(PhysicsError{"Static mesh contains a non-finite vertex."});
        jolt_vertices.emplace_back(vertex.x, vertex.y, vertex.z);
    }

    JPH::IndexedTriangleList triangles;
    triangles.reserve(triangle_indices.size() / 3);
    for (std::size_t i = 0; i < triangle_indices.size(); i += 3) {
        const auto a = triangle_indices[i];
        const auto b = triangle_indices[i + 1];
        const auto c = triangle_indices[i + 2];
        if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size() ||
            a == b || b == c || a == c) {
            return std::unexpected(PhysicsError{"Static mesh contains an invalid triangle index."});
        }
        triangles.emplace_back(a, b, c, 0);
    }

    JPH::MeshShapeSettings shape_settings{std::move(jolt_vertices), std::move(triangles)};
    shape_settings.mBuildQuality = JPH::MeshShapeSettings::EBuildQuality::FavorRuntimePerformance;
    auto shape_result = shape_settings.Create();
    if (shape_result.HasError())
        return std::unexpected(PhysicsError{"Jolt could not build static mesh: " +
                                            std::string{shape_result.GetError().c_str()}});

    JPH::BodyCreationSettings settings{
        shape_result.Get(), JPH::RVec3{position.x, position.y, position.z},
        JPH::Quat::sIdentity(), JPH::EMotionType::Static, kStaticLayer};
    const auto handle = impl_->next_handle++;
    settings.mUserData = handle;
    auto& body_interface = impl_->system.GetBodyInterface();
    const auto id = body_interface.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
    if (id.IsInvalid())
        return std::unexpected(PhysicsError{"Jolt could not allocate a static mesh body."});
    impl_->bodies.emplace(handle, id);
    impl_->contact_listener.add_body(BodyHandle{handle});
    return BodyHandle{handle};
}

bool PhysicsWorld::destroy_body(BodyHandle body) noexcept {
    if (!impl_)
        return false;
    const auto found = impl_->bodies.find(body.value);
    if (found == impl_->bodies.end())
        return false;
    for (auto constraint = impl_->constraints.begin(); constraint != impl_->constraints.end();) {
        if (constraint->second.body_a == body || constraint->second.body_b == body) {
            impl_->system.RemoveConstraint(constraint->second.constraint.GetPtr());
            constraint = impl_->constraints.erase(constraint);
        } else {
            ++constraint;
        }
    }
    auto& interface = impl_->system.GetBodyInterface();
    interface.RemoveBody(found->second);
    interface.DestroyBody(found->second);
    impl_->bodies.erase(found);
    impl_->contact_listener.remove_body(body);
    return true;
}

std::expected<ConstraintHandle, PhysicsError>
PhysicsWorld::create_constraint(const ConstraintDesc& desc) {
    if (!impl_ || desc.body_a == desc.body_b || !finite(desc.local_anchor_a) ||
        !finite(desc.local_anchor_b) || !finite(desc.local_axis_a) ||
        !finite(desc.local_axis_b) || !finite(desc.local_normal_a) ||
        !finite(desc.local_normal_b) || !std::isfinite(desc.min_limit) ||
        !std::isfinite(desc.max_limit) || desc.min_limit > desc.max_limit) {
        return std::unexpected(PhysicsError{"Constraint description contains invalid values."});
    }
    const auto body_a = impl_->bodies.find(desc.body_a.value);
    const auto body_b = impl_->bodies.find(desc.body_b.value);
    if (body_a == impl_->bodies.end() || body_b == impl_->bodies.end())
        return std::unexpected(PhysicsError{"Constraint references a body handle that is not valid."});

    constexpr float kAxisEpsilon = 1.0e-8F;
    const auto valid_axis = [](rhi::Vec3 axis) {
        return finite(axis) && length_squared(axis) > kAxisEpsilon;
    };
    if (desc.type == ConstraintType::fixed || desc.type == ConstraintType::hinge) {
        if (!valid_axis(desc.local_axis_a) || !valid_axis(desc.local_axis_b) ||
            !valid_axis(desc.local_normal_a) || !valid_axis(desc.local_normal_b) ||
            std::abs(dot(normalized(desc.local_axis_a), normalized(desc.local_normal_a))) >
                1.0e-3F ||
            std::abs(dot(normalized(desc.local_axis_b), normalized(desc.local_normal_b))) >
                1.0e-3F) {
            return std::unexpected(PhysicsError{"Constraint axes must be non-zero and perpendicular."});
        }
    }
    if (desc.type == ConstraintType::hinge &&
        (desc.min_limit < -3.14159265F || desc.max_limit > 3.14159265F ||
         desc.min_limit > 0.0F || desc.max_limit < 0.0F)) {
        return std::unexpected(PhysicsError{"Hinge limits must be in [-pi, 0] and [0, pi]."});
    }
    if (desc.type == ConstraintType::distance &&
        (desc.min_limit < 0.0F || desc.max_limit < 0.0F)) {
        return std::unexpected(PhysicsError{"Distance constraint limits must be non-negative."});
    }
    if (desc.type != ConstraintType::fixed && desc.type != ConstraintType::hinge &&
        desc.type != ConstraintType::distance) {
        return std::unexpected(PhysicsError{"Constraint type is not supported."});
    }

    JPH::Ref<JPH::TwoBodyConstraint> constraint;
    auto& body_interface = impl_->system.GetBodyInterface();
    switch (desc.type) {
    case ConstraintType::fixed: {
        JPH::FixedConstraintSettings settings;
        settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
        settings.mPoint1 = {desc.local_anchor_a.x, desc.local_anchor_a.y, desc.local_anchor_a.z};
        settings.mPoint2 = {desc.local_anchor_b.x, desc.local_anchor_b.y, desc.local_anchor_b.z};
        const auto normal_a = normalized(desc.local_normal_a);
        const auto axis_a = normalized(desc.local_axis_a);
        const auto normal_b = normalized(desc.local_normal_b);
        const auto axis_b = normalized(desc.local_axis_b);
        settings.mAxisX1 = {normal_a.x, normal_a.y, normal_a.z};
        settings.mAxisY1 = {axis_a.x, axis_a.y, axis_a.z};
        settings.mAxisX2 = {normal_b.x, normal_b.y, normal_b.z};
        settings.mAxisY2 = {axis_b.x, axis_b.y, axis_b.z};
        constraint = body_interface.CreateConstraint(&settings, body_a->second, body_b->second);
        break;
    }
    case ConstraintType::hinge: {
        JPH::HingeConstraintSettings settings;
        settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
        settings.mPoint1 = {desc.local_anchor_a.x, desc.local_anchor_a.y, desc.local_anchor_a.z};
        settings.mPoint2 = {desc.local_anchor_b.x, desc.local_anchor_b.y, desc.local_anchor_b.z};
        const auto axis_a = normalized(desc.local_axis_a);
        const auto axis_b = normalized(desc.local_axis_b);
        const auto normal_a = normalized(desc.local_normal_a);
        const auto normal_b = normalized(desc.local_normal_b);
        settings.mHingeAxis1 = {axis_a.x, axis_a.y, axis_a.z};
        settings.mHingeAxis2 = {axis_b.x, axis_b.y, axis_b.z};
        settings.mNormalAxis1 = {normal_a.x, normal_a.y, normal_a.z};
        settings.mNormalAxis2 = {normal_b.x, normal_b.y, normal_b.z};
        settings.mLimitsMin = desc.min_limit;
        settings.mLimitsMax = desc.max_limit;
        constraint = body_interface.CreateConstraint(&settings, body_a->second, body_b->second);
        break;
    }
    case ConstraintType::distance: {
        JPH::DistanceConstraintSettings settings;
        settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
        settings.mPoint1 = {desc.local_anchor_a.x, desc.local_anchor_a.y, desc.local_anchor_a.z};
        settings.mPoint2 = {desc.local_anchor_b.x, desc.local_anchor_b.y, desc.local_anchor_b.z};
        settings.mMinDistance = desc.min_limit;
        settings.mMaxDistance = desc.max_limit;
        constraint = body_interface.CreateConstraint(&settings, body_a->second, body_b->second);
        break;
    }
    }
    if (constraint == nullptr)
        return std::unexpected(PhysicsError{"Jolt could not create the requested constraint."});
    impl_->system.AddConstraint(constraint.GetPtr());
    const auto handle = impl_->next_constraint_handle++;
    impl_->constraints.emplace(handle, Impl::ConstraintRecord{
        .constraint = std::move(constraint), .body_a = desc.body_a, .body_b = desc.body_b});
    return ConstraintHandle{handle};
}

bool PhysicsWorld::destroy_constraint(ConstraintHandle constraint) noexcept {
    if (!impl_)
        return false;
    const auto found = impl_->constraints.find(constraint.value);
    if (found == impl_->constraints.end())
        return false;
    impl_->system.RemoveConstraint(found->second.constraint.GetPtr());
    impl_->constraints.erase(found);
    return true;
}

std::vector<ContactEvent> PhysicsWorld::drain_contact_events() {
    if (!impl_)
        return {};
    return impl_->contact_listener.drain();
}

std::expected<CharacterHandle, PhysicsError>
PhysicsWorld::create_character(const CharacterDesc& desc) {
    if (!impl_ || !finite(desc.position) || !finite(desc.rotation) ||
        !std::isfinite(desc.capsule_half_height) || desc.capsule_half_height <= 0.0F ||
        !std::isfinite(desc.capsule_radius) || desc.capsule_radius <= 0.0F ||
        !std::isfinite(desc.mass) || desc.mass <= 0.0F ||
        !std::isfinite(desc.max_strength) || desc.max_strength < 0.0F ||
        !std::isfinite(desc.max_slope_angle_radians) || desc.max_slope_angle_radians <= 0.0F ||
        desc.max_slope_angle_radians >= 1.57079633F) {
        return std::unexpected(PhysicsError{"Character description contains invalid values."});
    }
    if (impl_->characters.size() >= impl_->config.max_bodies)
        return std::unexpected(PhysicsError{"PhysicsWorld reached its configured character limit."});

    JPH::Ref<JPH::Shape> capsule = new JPH::CapsuleShape(
        desc.capsule_half_height, desc.capsule_radius);
    JPH::RotatedTranslatedShapeSettings translated_shape{
        {0.0F, desc.capsule_half_height + desc.capsule_radius, 0.0F},
        JPH::Quat::sIdentity(), capsule.GetPtr()};
    auto shape_result = translated_shape.Create();
    if (shape_result.HasError()) {
        return std::unexpected(PhysicsError{"Jolt could not create the character capsule: " +
                                            std::string{shape_result.GetError().c_str()}});
    }

    JPH::CharacterVirtualSettings settings;
    settings.mShape = shape_result.Get();
    settings.mMass = desc.mass;
    settings.mMaxStrength = desc.max_strength;
    settings.mMaxSlopeAngle = desc.max_slope_angle_radians;
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -desc.capsule_half_height);

    auto character = JPH::Ref<JPH::CharacterVirtual>{new JPH::CharacterVirtual(
        &settings, JPH::RVec3{desc.position.x, desc.position.y, desc.position.z},
        JPH::Quat{desc.rotation.x, desc.rotation.y, desc.rotation.z, desc.rotation.w},
        0, &impl_->system)};
    const auto handle = impl_->next_character_handle++;
    impl_->characters.emplace(handle, Impl::CharacterRecord{.character = std::move(character)});
    return CharacterHandle{handle};
}

bool PhysicsWorld::destroy_character(CharacterHandle character) noexcept {
    if (!impl_)
        return false;
    return impl_->characters.erase(character.value) != 0;
}

bool PhysicsWorld::set_horizontal_velocity(CharacterHandle character,
                                           rhi::Vec3 velocity) noexcept {
    if (!impl_ || !finite(velocity))
        return false;
    const auto found = impl_->characters.find(character.value);
    if (found == impl_->characters.end())
        return false;
    found->second.horizontal_velocity = {velocity.x, 0.0F, velocity.z};
    return true;
}

bool PhysicsWorld::jump(CharacterHandle character, float speed) noexcept {
    if (!impl_ || !std::isfinite(speed) || speed <= 0.0F)
        return false;
    const auto found = impl_->characters.find(character.value);
    if (found == impl_->characters.end())
        return false;
    found->second.jump_speed = speed;
    found->second.jump_requested = true;
    return true;
}

std::expected<CharacterState, PhysicsError>
PhysicsWorld::character_state(CharacterHandle character) const {
    if (!impl_)
        return std::unexpected(PhysicsError{"PhysicsWorld is not initialized."});
    const auto found = impl_->characters.find(character.value);
    if (found == impl_->characters.end())
        return std::unexpected(PhysicsError{"Character handle is not valid in this world."});
    const auto& virtual_character = *found->second.character;
    const auto position = virtual_character.GetPosition();
    const auto velocity = virtual_character.GetLinearVelocity();
    CharacterGroundState ground_state = CharacterGroundState::in_air;
    switch (virtual_character.GetGroundState()) {
    case JPH::CharacterBase::EGroundState::OnGround:
        ground_state = CharacterGroundState::on_ground;
        break;
    case JPH::CharacterBase::EGroundState::OnSteepGround:
        ground_state = CharacterGroundState::on_steep_ground;
        break;
    case JPH::CharacterBase::EGroundState::NotSupported:
        ground_state = CharacterGroundState::not_supported;
        break;
    case JPH::CharacterBase::EGroundState::InAir:
        ground_state = CharacterGroundState::in_air;
        break;
    }
    return CharacterState{
        .position = {static_cast<float>(position.GetX()), static_cast<float>(position.GetY()),
                     static_cast<float>(position.GetZ())},
        .linear_velocity = {velocity.GetX(), velocity.GetY(), velocity.GetZ()},
        .ground_state = ground_state,
    };
}

std::expected<void, PhysicsError> PhysicsWorld::step(float delta_time) {
    if (!impl_ || !std::isfinite(delta_time) || delta_time < 0.0F)
        return std::unexpected(PhysicsError{"PhysicsWorld::step requires a finite non-negative delta."});
    impl_->accumulator += std::min(delta_time, 0.25F);
    const float fixed_step = impl_->config.fixed_step_seconds;
    std::uint32_t steps = 0;
    while (impl_->accumulator >= fixed_step && steps < impl_->config.max_substeps) {
        const auto error = impl_->system.Update(fixed_step, 1, &impl_->temporary_allocator,
                                                &impl_->jobs);
        if (error != JPH::EPhysicsUpdateError::None)
            return std::unexpected(PhysicsError{"Jolt reported a physics update capacity error."});

        const auto broad_phase_filter = impl_->system.GetDefaultBroadPhaseLayerFilter(kMovingLayer);
        const auto object_layer_filter = impl_->system.GetDefaultLayerFilter(kMovingLayer);
        for (auto& [handle, record] : impl_->characters) {
            (void)handle;
            auto& character = *record.character;
            const JPH::Vec3 current_vertical_velocity{0.0F, character.GetLinearVelocity().GetY(), 0.0F};
            const auto ground_velocity = character.GetGroundVelocity();
            JPH::Vec3 velocity;
            if (character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround &&
                current_vertical_velocity.GetY() - ground_velocity.GetY() < 0.1F) {
                velocity = ground_velocity;
                if (record.jump_requested) {
                    velocity += JPH::Vec3{0.0F, record.jump_speed, 0.0F};
                    record.jump_requested = false;
                    record.jump_speed = 0.0F;
                }
            } else {
                velocity = current_vertical_velocity;
            }
            velocity += impl_->system.GetGravity() * fixed_step;
            velocity += JPH::Vec3{record.horizontal_velocity.x, 0.0F,
                                  record.horizontal_velocity.z};
            character.SetLinearVelocity(velocity);
            JPH::CharacterVirtual::ExtendedUpdateSettings update_settings;
            update_settings.mStickToFloorStepDown = {0.0F, -0.5F, 0.0F};
            update_settings.mWalkStairsStepUp = {0.0F, 0.4F, 0.0F};
            update_settings.mWalkStairsMinStepForward = 0.02F;
            update_settings.mWalkStairsStepForwardTest = 0.15F;
            character.ExtendedUpdate(fixed_step, impl_->system.GetGravity(), update_settings,
                                     broad_phase_filter, object_layer_filter, {}, {},
                                     impl_->temporary_allocator);
        }
        impl_->accumulator -= fixed_step;
        ++steps;
    }
    if (steps == impl_->config.max_substeps)
        impl_->accumulator = std::min(impl_->accumulator, fixed_step);
    return {};
}

std::expected<BodyState, PhysicsError> PhysicsWorld::body_state(BodyHandle body) const {
    if (!impl_)
        return std::unexpected(PhysicsError{"PhysicsWorld is not initialized."});
    const auto found = impl_->bodies.find(body.value);
    if (found == impl_->bodies.end())
        return std::unexpected(PhysicsError{"Rigid body handle is not valid in this world."});
    const auto& interface = impl_->system.GetBodyInterface();
    const auto position = interface.GetPosition(found->second);
    const auto rotation = interface.GetRotation(found->second);
    const auto velocity = interface.GetLinearVelocity(found->second);
    return BodyState{
        .position = {static_cast<float>(position.GetX()), static_cast<float>(position.GetY()),
                     static_cast<float>(position.GetZ())},
        .rotation = {rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()},
        .linear_velocity = {velocity.GetX(), velocity.GetY(), velocity.GetZ()},
    };
}

std::expected<std::optional<RayHit>, PhysicsError>
PhysicsWorld::raycast(rhi::Vec3 origin, rhi::Vec3 direction, float max_distance) const {
    if (!impl_ || !finite(origin) || !finite(direction) || !std::isfinite(max_distance) ||
        max_distance <= 0.0F) {
        return std::unexpected(PhysicsError{"Raycast requires finite origin/direction and positive distance."});
    }
    const float length_squared = direction.x * direction.x + direction.y * direction.y +
                                 direction.z * direction.z;
    if (!std::isfinite(length_squared) || length_squared <= 1.0e-12F)
        return std::unexpected(PhysicsError{"Raycast direction must be non-zero."});
    const float inverse_length = 1.0F / std::sqrt(length_squared);
    const JPH::Vec3 displacement{direction.x * inverse_length * max_distance,
                                 direction.y * inverse_length * max_distance,
                                 direction.z * inverse_length * max_distance};
    const JPH::RRayCast ray{JPH::RVec3{origin.x, origin.y, origin.z}, displacement};
    JPH::RayCastResult result;
    if (!impl_->system.GetNarrowPhaseQuery().CastRay(ray, result))
        return std::optional<RayHit>{};

    const auto found = std::find_if(impl_->bodies.begin(), impl_->bodies.end(),
                                    [&result](const auto& entry) {
                                        return entry.second == result.mBodyID;
                                    });
    if (found == impl_->bodies.end())
        return std::optional<RayHit>{};
    const float distance = result.mFraction * max_distance;
    return std::optional<RayHit>{RayHit{
        .body = BodyHandle{found->first},
        .position = {origin.x + direction.x * inverse_length * distance,
                     origin.y + direction.y * inverse_length * distance,
                     origin.z + direction.z * inverse_length * distance},
        .distance = distance,
    }};
}

bool PhysicsWorld::set_linear_velocity(BodyHandle body, rhi::Vec3 velocity) noexcept {
    if (!impl_ || !finite(velocity))
        return false;
    const auto found = impl_->bodies.find(body.value);
    if (found == impl_->bodies.end())
        return false;
    impl_->system.GetBodyInterface().SetLinearVelocity(found->second,
                                                        {velocity.x, velocity.y, velocity.z});
    return true;
}

bool PhysicsWorld::apply_impulse(BodyHandle body, rhi::Vec3 impulse) noexcept {
    if (!impl_ || !finite(impulse))
        return false;
    const auto found = impl_->bodies.find(body.value);
    if (found == impl_->bodies.end())
        return false;
    impl_->system.GetBodyInterface().AddImpulse(found->second, {impulse.x, impulse.y, impulse.z});
    return true;
}

float PhysicsWorld::interpolation_alpha() const noexcept {
    if (!impl_)
        return 0.0F;
    return std::clamp(impl_->accumulator / impl_->config.fixed_step_seconds, 0.0F, 1.0F);
}

std::uint32_t PhysicsWorld::body_count() const noexcept {
    return impl_ ? static_cast<std::uint32_t>(impl_->bodies.size()) : 0U;
}

} // namespace astra::physics

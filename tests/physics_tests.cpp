#include <astra/physics/physics_world.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>

int main() {
    using namespace astra::physics;
    auto world_result = PhysicsWorld::create({.worker_threads = 1});
    assert(world_result);
    auto world = std::move(*world_result);
    assert(!PhysicsWorld::create({.max_contact_events = 0}));

    const auto floor = world.create_body({
        .shape = {ShapeType::box, {20.0F, 0.5F, 20.0F}},
        .position = {0.0F, -0.5F, 0.0F},
        .motion = MotionType::static_body,
    });
    const auto ball = world.create_body({
        .shape = {ShapeType::sphere, {0.5F, 0.5F, 0.5F}},
        .position = {0.0F, 4.0F, 0.0F},
        .restitution = 0.1F,
    });
    assert(floor && ball);
    assert(world.body_count() == 2);
    assert(!world.create_constraint({.body_a = *ball, .body_b = BodyHandle{999999}}));
    assert(!world.create_constraint({.body_a = *ball, .body_b = *ball}));

    const auto fixed_body = world.create_body({
        .shape = {ShapeType::box, {0.4F, 0.4F, 0.4F}},
        .position = {20.0F, 3.0F, 0.0F},
    });
    const auto fixed_other = world.create_body({
        .shape = {ShapeType::box, {0.4F, 0.4F, 0.4F}},
        .position = {22.0F, 3.0F, 0.0F},
    });
    assert(fixed_body && fixed_other);
    const auto fixed = world.create_constraint({
        .type = ConstraintType::fixed, .body_a = *fixed_body, .body_b = *fixed_other,
    });
    assert(fixed);
    assert(world.destroy_constraint(*fixed));
    assert(!world.destroy_constraint(*fixed));

    const auto hinge_body = world.create_body({
        .shape = {ShapeType::box, {0.4F, 0.4F, 0.4F}},
        .position = {30.0F, 3.0F, 0.0F},
    });
    const auto hinge_other = world.create_body({
        .shape = {ShapeType::box, {0.4F, 0.4F, 0.4F}},
        .position = {32.0F, 3.0F, 0.0F},
    });
    assert(hinge_body && hinge_other);
    const auto hinge = world.create_constraint({
        .type = ConstraintType::hinge, .body_a = *hinge_body, .body_b = *hinge_other,
        .min_limit = -1.0F, .max_limit = 1.0F,
    });
    assert(hinge);
    assert(!world.create_constraint({
        .type = ConstraintType::hinge, .body_a = *hinge_body, .body_b = *hinge_other,
        .local_axis_a = {0.0F, 1.0F, 0.0F}, .local_normal_a = {0.0F, 1.0F, 0.0F},
    }));
    assert(world.destroy_constraint(*hinge));

    const auto distance_body = world.create_body({
        .shape = {ShapeType::sphere, {0.3F, 0.3F, 0.3F}},
        .position = {40.0F, 3.0F, 0.0F},
    });
    const auto distance_other = world.create_body({
        .shape = {ShapeType::sphere, {0.3F, 0.3F, 0.3F}},
        .position = {42.0F, 3.0F, 0.0F},
    });
    assert(distance_body && distance_other);
    const auto distance = world.create_constraint({
        .type = ConstraintType::distance, .body_a = *distance_body,
        .body_b = *distance_other, .min_limit = 1.0F, .max_limit = 3.0F,
    });
    assert(distance);
    assert(!world.create_constraint({
        .type = ConstraintType::distance, .body_a = *distance_body,
        .body_b = *distance_other, .min_limit = 3.0F, .max_limit = 1.0F,
    }));
    assert(world.destroy_constraint(*distance));

    const auto auto_removed = world.create_constraint({
        .type = ConstraintType::fixed, .body_a = *fixed_body, .body_b = *fixed_other,
    });
    assert(auto_removed);
    assert(world.destroy_body(*fixed_body));
    assert(!world.destroy_constraint(*auto_removed));
    assert(world.destroy_body(*fixed_other));
    assert(world.destroy_body(*hinge_body));
    assert(world.destroy_body(*hinge_other));
    assert(world.destroy_body(*distance_body));
    assert(world.destroy_body(*distance_other));

    for (int frame = 0; frame < 240; ++frame)
        assert(world.step(1.0F / 60.0F));

    const auto state = world.body_state(*ball);
    assert(state);
    assert(std::isfinite(state->position.y));
    assert(state->position.y > 0.2F && state->position.y < 1.0F);
    const auto contacts = world.drain_contact_events();
    assert(!contacts.empty());
    assert(std::any_of(contacts.begin(), contacts.end(), [floor = *floor, ball = *ball](const ContactEvent& event) {
        return (event.body_a == floor && event.body_b == ball) ||
               (event.body_a == ball && event.body_b == floor);
    }));
    for (const auto& event : contacts) {
        assert(std::isfinite(event.position.x) && std::isfinite(event.position.y) &&
               std::isfinite(event.position.z));
        assert(std::isfinite(event.normal.x) && std::isfinite(event.normal.y) &&
               std::isfinite(event.normal.z));
        assert(std::isfinite(event.penetration_depth));
    }
    assert(world.drain_contact_events().empty());
    assert(world.apply_impulse(*ball, {0.0F, 1.0F, 0.0F}));
    assert(world.destroy_body(*ball));
    const auto after_ball_destroy = world.drain_contact_events();
    assert(std::none_of(after_ball_destroy.begin(), after_ball_destroy.end(),
                        [ball = *ball](const ContactEvent& event) {
                            return event.body_a == ball || event.body_b == ball;
                        }));
    assert(!world.destroy_body(*ball));
    assert(world.body_count() == 1);
    assert(world.destroy_body(*floor));

    // Static triangle meshes are the collision representation used for
    // streamed terrain chunks and level geometry.
    const astra::rhi::Vec3 terrain_vertices[] = {
        {-8.0F, 0.0F, -8.0F}, {-8.0F, 0.0F, 8.0F},
        {8.0F, 0.0F, -8.0F}, {8.0F, 0.0F, 8.0F},
    };
    const std::uint32_t terrain_indices[] = {0, 1, 2, 2, 1, 3};
    const auto terrain = world.create_static_mesh(terrain_vertices, terrain_indices);
    const auto terrain_ball = world.create_body({
        .shape = {ShapeType::sphere, {0.4F, 0.4F, 0.4F}},
        .position = {0.0F, 3.0F, 0.0F},
        .restitution = 0.0F,
    });
    assert(terrain && terrain_ball);
    for (int frame = 0; frame < 240; ++frame)
        assert(world.step(1.0F / 60.0F));
    const auto terrain_state = world.body_state(*terrain_ball);
    assert(terrain_state && terrain_state->position.y > 0.25F && terrain_state->position.y < 0.8F);
    const auto terrain_contacts = world.drain_contact_events();
    assert(std::any_of(terrain_contacts.begin(), terrain_contacts.end(),
                       [terrain = *terrain, ball = *terrain_ball](const ContactEvent& event) {
                           return (event.body_a == terrain && event.body_b == ball) ||
                                  (event.body_a == ball && event.body_b == terrain);
                       }));
    assert(world.step(1.0F / 60.0F));
    assert(world.destroy_body(*terrain_ball));
    const auto stale_terrain_contacts = world.drain_contact_events();
    assert(std::none_of(stale_terrain_contacts.begin(), stale_terrain_contacts.end(),
                        [ball = *terrain_ball](const ContactEvent& event) {
                            return event.body_a == ball || event.body_b == ball;
                        }));

    // CharacterVirtual uses a bottom-origin capsule and queries the same
    // moving-vs-static filters as rigid bodies.
    const auto character = world.create_character({.position = {0.0F, 3.0F, 0.0F}});
    assert(character);
    for (int frame = 0; frame < 240; ++frame)
        assert(world.step(1.0F / 60.0F));
    auto character_on_ground = world.character_state(*character);
    assert(character_on_ground);
    assert(character_on_ground->ground_state == CharacterGroundState::on_ground);
    assert(std::abs(character_on_ground->position.y) < 0.1F);

    assert(world.set_horizontal_velocity(*character, {2.0F, 50.0F, 0.0F}));
    for (int frame = 0; frame < 60; ++frame)
        assert(world.step(1.0F / 60.0F));
    const auto character_moved = world.character_state(*character);
    assert(character_moved && character_moved->position.x > 1.5F);
    assert(std::abs(character_moved->position.y) < 0.1F);

    assert(world.set_horizontal_velocity(*character, {}));
    assert(world.jump(*character, 5.0F));
    for (int frame = 0; frame < 18; ++frame)
        assert(world.step(1.0F / 60.0F));
    const auto character_jumped = world.character_state(*character);
    assert(character_jumped && character_jumped->position.y > 0.5F);
    assert(character_jumped->linear_velocity.y > 0.0F);

    assert(!world.create_character({.capsule_half_height = -0.1F}));
    assert(!world.create_character({.capsule_radius = 0.0F}));
    assert(!world.create_character({.max_slope_angle_radians = 2.0F}));
    assert(!world.jump(*character, 0.0F));
    assert(!world.set_horizontal_velocity(CharacterHandle{999999}, {1.0F, 0.0F, 0.0F}));
    assert(world.destroy_character(*character));
    assert(!world.destroy_character(*character));
    assert(!world.character_state(*character));

    const auto hit = world.raycast({0.0F, 5.0F, 0.0F}, {0.0F, -2.0F, 0.0F}, 10.0F);
    assert(hit && *hit && (*hit)->body == *terrain);
    assert(std::abs((*hit)->position.y) < 0.05F);
    const auto miss = world.raycast({0.0F, 5.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, 10.0F);
    assert(miss && !*miss);
    assert(world.destroy_body(*terrain));

    const std::uint32_t invalid_indices[] = {0, 1, 9};
    assert(!world.create_static_mesh(terrain_vertices, invalid_indices));

    // A fixed constraint must preserve the pair's relative transform under load,
    // not merely survive creation and destruction.
    auto constraint_world_result = PhysicsWorld::create({.gravity = {}, .worker_threads = 1});
    assert(constraint_world_result);
    auto constraint_world = std::move(*constraint_world_result);
    const auto constrained_a = constraint_world.create_body({
        .shape = {ShapeType::sphere, {0.3F, 0.3F, 0.3F}},
        .position = {0.0F, 0.0F, 0.0F},
    });
    const auto constrained_b = constraint_world.create_body({
        .shape = {ShapeType::sphere, {0.3F, 0.3F, 0.3F}},
        .position = {2.0F, 0.0F, 0.0F},
    });
    assert(constrained_a && constrained_b);
    const auto fixed_joint = constraint_world.create_constraint({
        .type = ConstraintType::fixed,
        .body_a = *constrained_a,
        .body_b = *constrained_b,
        .local_anchor_a = {1.0F, 0.0F, 0.0F},
        .local_anchor_b = {-1.0F, 0.0F, 0.0F},
    });
    assert(fixed_joint);
    assert(constraint_world.apply_impulse(*constrained_a, {5.0F, 2.0F, 0.0F}));
    for (int frame = 0; frame < 120; ++frame)
        assert(constraint_world.step(1.0F / 60.0F));
    const auto constrained_state_a = constraint_world.body_state(*constrained_a);
    const auto constrained_state_b = constraint_world.body_state(*constrained_b);
    assert(constrained_state_a && constrained_state_b);
    const float constrained_dx = constrained_state_b->position.x - constrained_state_a->position.x;
    const float constrained_dy = constrained_state_b->position.y - constrained_state_a->position.y;
    const float constrained_dz = constrained_state_b->position.z - constrained_state_a->position.z;
    assert(std::abs(std::sqrt(constrained_dx * constrained_dx + constrained_dy * constrained_dy +
                              constrained_dz * constrained_dz) - 2.0F) < 0.05F);

    // Contact collection remains bounded if a consumer forgets to drain events.
    auto bounded_world_result = PhysicsWorld::create({
        .max_contact_events = 1,
        .worker_threads = 1,
    });
    assert(bounded_world_result);
    auto bounded_world = std::move(*bounded_world_result);
    assert(bounded_world.create_body({
        .shape = {ShapeType::box, {5.0F, 0.5F, 5.0F}},
        .position = {0.0F, -0.5F, 0.0F},
        .motion = MotionType::static_body,
    }));
    assert(bounded_world.create_body({
        .shape = {ShapeType::sphere, {0.5F, 0.5F, 0.5F}},
        .position = {0.0F, 1.0F, 0.0F},
    }));
    for (int frame = 0; frame < 120; ++frame)
        assert(bounded_world.step(1.0F / 60.0F));
    const auto bounded_events = bounded_world.drain_contact_events();
    assert(bounded_events.size() == 1);
    return 0;
}

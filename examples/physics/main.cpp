#include <astra/physics/physics_world.hpp>

#include <cstdio>

int main() {
    using namespace astra::physics;
    auto world_result = PhysicsWorld::create({.worker_threads = 2});
    if (!world_result) {
        std::fprintf(stderr, "Physics initialization failed: %s\n", world_result.error().message.c_str());
        return 1;
    }
    auto world = std::move(*world_result);
    const auto floor = world.create_body({
        .shape = {ShapeType::box, {12.0F, 0.5F, 12.0F}},
        .position = {0.0F, -0.5F, 0.0F},
        .motion = MotionType::static_body,
        .friction = 0.9F,
    });
    if (!floor) {
        std::fprintf(stderr, "Floor creation failed: %s\n", floor.error().message.c_str());
        return 1;
    }
    for (std::uint32_t index = 0; index < 12; ++index) {
        BodyDesc body;
        body.shape = index % 2U == 0U ? Shape{ShapeType::box, {0.35F, 0.35F, 0.35F}}
                                     : Shape{ShapeType::sphere, {0.35F, 0.35F, 0.35F}};
        body.position = {static_cast<float>(index % 4U) - 1.5F,
                         1.0F + static_cast<float>(index / 4U) * 1.4F,
                         static_cast<float>(index / 4U) * 0.25F};
        body.mass = 1.0F + static_cast<float>(index) * 0.15F;
        body.friction = 0.72F;
        body.restitution = 0.12F;
        if (const auto created = world.create_body(body); !created) {
            std::fprintf(stderr, "Body creation failed: %s\n", created.error().message.c_str());
            return 1;
        }
    }

    for (int frame = 0; frame < 240; ++frame) {
        if (const auto result = world.step(1.0F / 60.0F); !result) {
            std::fprintf(stderr, "Physics step failed: %s\n", result.error().message.c_str());
            return 1;
        }
    }
    std::printf("Simulated %u rigid bodies for 4 seconds; fixed-step interpolation alpha %.3f\n",
                world.body_count(), world.interpolation_alpha());
    return 0;
}

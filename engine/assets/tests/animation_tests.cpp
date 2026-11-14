#include <astra/assets/animation.hpp>

#include <array>
#include <cassert>
#include <cmath>

using namespace astra::assets;

namespace {
bool near(float a, float b, float epsilon = 1.0e-4F) { return std::abs(a-b) <= epsilon; }

Animation make_animation(Interpolation interpolation, std::uint32_t components,
                         std::vector<float> times, std::vector<float> values,
                         AnimationPath path = AnimationPath::translation) {
    Animation animation;
    animation.samplers.push_back({interpolation, std::move(times), components, std::move(values)});
    animation.channels.push_back({0, 0, path});
    return animation;
}
}

int main() {
    std::array<Node, 1> nodes{};
    nodes[0].translation = {2, 3, 4};
    nodes[0].scale = {2, 2, 2};

    auto step = make_animation(Interpolation::step, 3, {0, 1}, {0,0,0, 10,20,30});
    auto stepped = evaluate_animation(step, nodes, 0.75F);
    assert(stepped && near((*stepped)[0].translation.x, 0));
    assert(near((*stepped)[0].scale.x, 2));

    auto linear = make_animation(Interpolation::linear, 3, {0, 2}, {0,2,4, 4,6,8});
    auto halfway = evaluate_animation(linear, nodes, 1.0F);
    assert(halfway && near((*halfway)[0].translation.x, 2));
    assert(near((*halfway)[0].translation.y, 4));
    assert(near((*halfway)[0].translation.z, 6));
    auto clamped = evaluate_animation(linear, nodes, -5.0F);
    assert(clamped && near((*clamped)[0].translation.x, 0));

    constexpr float half_sqrt2 = 0.70710678118F;
    auto rotation = make_animation(Interpolation::linear, 4, {0,1},
        {0,0,0,1, 0,0,half_sqrt2,half_sqrt2}, AnimationPath::rotation);
    auto rotated = evaluate_animation(rotation, nodes, 0.5F);
    assert(rotated && near((*rotated)[0].rotation.z, std::sin(3.14159265359F/8.0F)));
    assert(near((*rotated)[0].rotation.w, std::cos(3.14159265359F/8.0F)));

    // Hermite with unit-duration endpoints and zero tangents gives smoothstep(0.5)=0.5.
    auto cubic = make_animation(Interpolation::cubic_spline, 3, {0,1},
        {0,0,0, 0,0,0, 0,0,0, 0,0,0, 10,0,0, 0,0,0});
    auto curved = evaluate_animation(cubic, nodes, 0.5F);
    assert(curved && near((*curved)[0].translation.x, 5));

    auto bad_time = evaluate_animation(linear, nodes, NAN);
    assert(!bad_time);
    auto weights = make_animation(Interpolation::linear, 1, {0}, {1}, AnimationPath::weights);
    assert(!evaluate_animation(weights, nodes, 0));
    nodes[0].has_matrix = true;
    assert(!evaluate_animation(linear, nodes, 0.5F));
}

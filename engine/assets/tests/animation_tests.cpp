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
    nodes[0].weights = {0.0F};
    weights.channels[0].weight_count = 1;
    auto sampled_weight = evaluate_animation(weights, nodes, 0);
    assert(sampled_weight && sampled_weight->front().morph_weights.size() == 1);
    assert(near(sampled_weight->front().morph_weights[0], 1.0F));

    auto single_target_linear = make_animation(Interpolation::linear, 1, {0, 1},
        {0.0F, 1.0F}, AnimationPath::weights);
    single_target_linear.channels[0].weight_count = 1;
    auto single_target_halfway = evaluate_animation(single_target_linear, nodes, 0.5F);
    assert(single_target_halfway &&
           near(single_target_halfway->front().morph_weights.front(), 0.5F));

    nodes[0].weights = {0.0F, 0.0F};
    auto morph_linear = make_animation(Interpolation::linear, 1, {0, 2},
        {0.0F, 0.2F, 1.0F, 0.8F}, AnimationPath::weights);
    morph_linear.channels[0].weight_count = 2;
    auto morph_halfway = evaluate_animation(morph_linear, nodes, 1.0F);
    assert(morph_halfway && near(morph_halfway->front().morph_weights[0], 0.5F));
    assert(near(morph_halfway->front().morph_weights[1], 0.5F));

    auto morph_step = make_animation(Interpolation::step, 1, {0, 1},
        {0.1F, 0.2F, 0.8F, 0.9F}, AnimationPath::weights);
    morph_step.channels[0].weight_count = 2;
    auto stepped_weights = evaluate_animation(morph_step, nodes, 0.75F);
    assert(stepped_weights && near(stepped_weights->front().morph_weights[0], 0.1F));
    assert(near(stepped_weights->front().morph_weights[1], 0.2F));

    auto morph_cubic = make_animation(Interpolation::cubic_spline, 1, {0, 1},
        {0,0, 0,0, 0,0, 0,0, 1,0, 0,0}, AnimationPath::weights);
    morph_cubic.channels[0].weight_count = 2;
    auto cubic_weights = evaluate_animation(morph_cubic, nodes, 0.5F);
    assert(cubic_weights && near(cubic_weights->front().morph_weights[0], 0.5F));
    assert(near(cubic_weights->front().morph_weights[1], 0.0F));

    morph_linear.channels[0].weight_count = 1;
    assert(!evaluate_animation(morph_linear, nodes, 1.0F));
    nodes[0].has_matrix = true;
    assert(!evaluate_animation(linear, nodes, 0.5F));
    nodes[0].weights = {0.0F};
    assert(evaluate_animation(weights, nodes, 0.0F));
}

#pragma once

#include <astra/assets/gltf.hpp>

#include <algorithm>
#include <cmath>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace astra::assets {

struct NodePose final {
    Vec3 translation{};
    Vec4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
    Vec3 scale{1.0F, 1.0F, 1.0F};
};

struct AnimationError final {
    std::string message;
};

namespace animation_detail {

inline Vec4 normalize(Vec4 q) noexcept {
    const float length_squared = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
    if (!(length_squared > 1.0e-20F) || !std::isfinite(length_squared))
        return {0.0F, 0.0F, 0.0F, 1.0F};
    const float inverse_length = 1.0F / std::sqrt(length_squared);
    return {q.x*inverse_length, q.y*inverse_length, q.z*inverse_length, q.w*inverse_length};
}

inline Vec4 slerp(Vec4 a, Vec4 b, float t) noexcept {
    a = normalize(a);
    b = normalize(b);
    float cosine = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    if (cosine < 0.0F) {
        b = {-b.x, -b.y, -b.z, -b.w};
        cosine = -cosine;
    }
    cosine = std::clamp(cosine, -1.0F, 1.0F);
    if (cosine > 0.9995F) {
        return normalize({a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t,
                          a.z+(b.z-a.z)*t, a.w+(b.w-a.w)*t});
    }
    const float angle = std::acos(cosine);
    const float sine = std::sin(angle);
    const float wa = std::sin((1.0F-t)*angle) / sine;
    const float wb = std::sin(t*angle) / sine;
    return normalize({a.x*wa+b.x*wb, a.y*wa+b.y*wb,
                      a.z*wa+b.z*wb, a.w*wa+b.w*wb});
}

inline float hermite(float p0, float m0, float p1, float m1, float t, float dt) noexcept {
    const float t2 = t*t;
    const float t3 = t2*t;
    return (2.0F*t3-3.0F*t2+1.0F)*p0 + (t3-2.0F*t2+t)*dt*m0
         + (-2.0F*t3+3.0F*t2)*p1 + (t3-t2)*dt*m1;
}

inline Vec4 read4(const std::vector<float>& values, std::size_t offset) noexcept {
    return {values[offset], values[offset+1], values[offset+2], values[offset+3]};
}

inline Vec3 read3(const std::vector<float>& values, std::size_t offset) noexcept {
    return {values[offset], values[offset+1], values[offset+2]};
}

} // namespace animation_detail

// Evaluates one glTF animation at an absolute clip time. The returned pose has
// one entry per asset node and starts with each node's authored TRS transform.
// Matrix-authored nodes and morph-weight channels are rejected because they
// cannot be represented by this TRS-only pose type.
[[nodiscard]] inline std::expected<std::vector<NodePose>, AnimationError>
evaluate_animation(const Animation& animation, std::span<const Node> nodes, float time) {
    using namespace animation_detail;
    if (!std::isfinite(time)) return std::unexpected(AnimationError{"animation time must be finite"});

    std::vector<NodePose> pose;
    pose.reserve(nodes.size());
    for (const auto& node : nodes) {
        if (node.has_matrix)
            pose.push_back({});
        else
            pose.push_back({node.translation, normalize(node.rotation), node.scale});
    }

    for (const auto& channel : animation.channels) {
        if (channel.node >= nodes.size())
            return std::unexpected(AnimationError{"animation channel node index is out of range"});
        if (channel.sampler >= animation.samplers.size())
            return std::unexpected(AnimationError{"animation channel sampler index is out of range"});
        if (channel.path != AnimationPath::translation && channel.path != AnimationPath::rotation &&
            channel.path != AnimationPath::scale && channel.path != AnimationPath::weights)
            return std::unexpected(AnimationError{"unknown animation target path"});
        if (channel.path == AnimationPath::weights)
            return std::unexpected(AnimationError{"morph-weight animation is not supported by TRS pose evaluation"});
        if (nodes[channel.node].has_matrix)
            return std::unexpected(AnimationError{"cannot animate a node authored with a matrix transform"});

        const auto& sampler = animation.samplers[channel.sampler];
        const std::size_t components = channel.path == AnimationPath::rotation ? 4U : 3U;
        const std::size_t factor = sampler.interpolation == Interpolation::cubic_spline ? 3U : 1U;
        if (sampler.components != components || sampler.times.empty() ||
            sampler.times.size() > std::numeric_limits<std::size_t>::max()/(components*factor) ||
            sampler.values.size() != sampler.times.size()*components*factor)
            return std::unexpected(AnimationError{"animation sampler has an invalid shape for its channel"});
        for (std::size_t i = 0; i < sampler.times.size(); ++i) {
            if (!std::isfinite(sampler.times[i]) || sampler.times[i] < 0.0F ||
                (i && !(sampler.times[i] > sampler.times[i-1])))
                return std::unexpected(AnimationError{"animation key times must be finite, nonnegative, and strictly increasing"});
        }
        if (sampler.interpolation != Interpolation::step &&
            sampler.interpolation != Interpolation::linear &&
            sampler.interpolation != Interpolation::cubic_spline)
            return std::unexpected(AnimationError{"unknown animation interpolation mode"});

        const auto key_value_offset = [&](std::size_t key) {
            return (key*factor + (factor == 3U ? 1U : 0U))*components;
        };
        std::size_t left = 0;
        std::size_t right = 0;
        float t = 0.0F;
        float dt = 0.0F;
        if (time <= sampler.times.front()) {
            left = right = 0;
        } else if (time >= sampler.times.back()) {
            left = right = sampler.times.size()-1;
        } else {
            const auto upper = std::upper_bound(sampler.times.begin(), sampler.times.end(), time);
            right = static_cast<std::size_t>(upper-sampler.times.begin());
            left = right-1;
            dt = sampler.times[right]-sampler.times[left];
            t = (time-sampler.times[left])/dt;
        }

        const bool rotation = channel.path == AnimationPath::rotation;
        const auto a_offset = key_value_offset(left);
        const auto b_offset = key_value_offset(right);
        Vec3 value3{};
        Vec4 value4{};
        if (left == right || sampler.interpolation == Interpolation::step) {
            if (rotation) value4 = read4(sampler.values, a_offset);
            else value3 = read3(sampler.values, a_offset);
        } else if (sampler.interpolation == Interpolation::linear) {
            if (rotation) {
                value4 = slerp(read4(sampler.values, a_offset), read4(sampler.values, b_offset), t);
            } else {
                const auto a = read3(sampler.values, a_offset);
                const auto b = read3(sampler.values, b_offset);
                value3 = {a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t};
            }
        } else {
            const auto out_tangent = a_offset + components;
            const auto in_tangent = b_offset - components;
            if (rotation) {
                const auto p0 = read4(sampler.values, a_offset);
                const auto m0 = read4(sampler.values, out_tangent);
                const auto p1 = read4(sampler.values, b_offset);
                const auto m1 = read4(sampler.values, in_tangent);
                value4 = normalize({hermite(p0.x,m0.x,p1.x,m1.x,t,dt),
                                    hermite(p0.y,m0.y,p1.y,m1.y,t,dt),
                                    hermite(p0.z,m0.z,p1.z,m1.z,t,dt),
                                    hermite(p0.w,m0.w,p1.w,m1.w,t,dt)});
            } else {
                const auto p0 = read3(sampler.values, a_offset);
                const auto m0 = read3(sampler.values, out_tangent);
                const auto p1 = read3(sampler.values, b_offset);
                const auto m1 = read3(sampler.values, in_tangent);
                value3 = {hermite(p0.x,m0.x,p1.x,m1.x,t,dt),
                          hermite(p0.y,m0.y,p1.y,m1.y,t,dt),
                          hermite(p0.z,m0.z,p1.z,m1.z,t,dt)};
            }
        }

        auto& target = pose[channel.node];
        switch (channel.path) {
        case AnimationPath::translation: target.translation = value3; break;
        case AnimationPath::rotation: target.rotation = normalize(value4); break;
        case AnimationPath::scale: target.scale = value3; break;
        case AnimationPath::weights: break;
        }
    }
    return pose;
}

} // namespace astra::assets

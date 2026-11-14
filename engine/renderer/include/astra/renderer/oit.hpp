#pragma once

#include <astra/rhi/types.hpp>

#include <algorithm>
#include <cmath>

namespace astra::renderer {

struct WeightedOitAccumulator final {
    float red{};
    float green{};
    float blue{};
    float weighted_alpha{};
    float revealage{1.0F};

    void accumulate(rhi::Color color, float alpha, float normalized_depth) noexcept {
        alpha = std::clamp(alpha, 0.0F, 1.0F);
        normalized_depth = std::clamp(normalized_depth, 0.0F, 1.0F);
        if (!std::isfinite(alpha) || !std::isfinite(normalized_depth) || alpha <= 0.0F)
            return;
        const float alpha_weight = std::min(1.0F, alpha * 10.0F) + 0.01F;
        const float depth_weight = 1.0F - normalized_depth * 0.9F;
        const float weight = std::clamp(alpha_weight * alpha_weight * alpha_weight * 1.0e8F *
                                            depth_weight * depth_weight * depth_weight,
                                        1.0e-2F, 3.0e3F);
        red += color.red * alpha * weight;
        green += color.green * alpha * weight;
        blue += color.blue * alpha * weight;
        weighted_alpha += alpha * weight;
        revealage *= 1.0F - alpha;
    }

    [[nodiscard]] rhi::Color composite(rhi::Color opaque) const noexcept {
        const float coverage = std::clamp(1.0F - revealage, 0.0F, 1.0F);
        if (coverage <= 0.0F || weighted_alpha <= 1.0e-8F)
            return opaque;
        const float inverse_alpha = 1.0F / weighted_alpha;
        const float inverse_coverage = 1.0F - coverage;
        return {
            red * inverse_alpha * coverage + opaque.red * inverse_coverage,
            green * inverse_alpha * coverage + opaque.green * inverse_coverage,
            blue * inverse_alpha * coverage + opaque.blue * inverse_coverage,
            opaque.alpha,
        };
    }
};

} // namespace astra::renderer

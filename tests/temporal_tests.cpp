#include <astra/renderer/temporal.hpp>

#include <cassert>
#include <cmath>

int main() {
    using astra::renderer::halton;
    using astra::renderer::jitter_projection;
    using astra::renderer::temporal_jitter;

    assert(std::abs(halton(1U, 2U) - 0.5F) < 0.00001F);
    assert(std::abs(halton(2U, 2U) - 0.25F) < 0.00001F);
    assert(std::abs(halton(1U, 3U) - (1.0F / 3.0F)) < 0.00001F);

    const auto first = temporal_jitter(0U, {1920U, 1080U});
    const auto second = temporal_jitter(1U, {1920U, 1080U});
    const auto repeated = temporal_jitter(8U, {1920U, 1080U});
    assert(first.x != second.x || first.y != second.y);
    assert(first.x == repeated.x && first.y == repeated.y);
    assert(std::abs(first.x) <= 1.0F / 1920.0F);
    assert(std::abs(first.y) <= 1.0F / 1080.0F);

    const auto zero_width = temporal_jitter(0U, {0U, 1080U});
    const auto zero_height = temporal_jitter(0U, {1920U, 0U});
    assert(zero_width.x == 0.0F && zero_width.y == 0.0F);
    assert(zero_height.x == 0.0F && zero_height.y == 0.0F);
    assert(halton(12U, 1U) == 0.0F);

    astra::rhi::Matrix4 projection{};
    projection.values[0] = 2.0F;
    projection.values[5] = 3.0F;
    projection.values[10] = 4.0F;
    projection.values[11] = -1.0F;
    projection.values[14] = -0.2F;
    const auto jittered = jitter_projection(projection, {0.01F, -0.02F});
    assert(std::abs(jittered.values[0] - 2.0F) < 0.00001F);
    assert(std::abs(jittered.values[5] - 3.0F) < 0.00001F);
    assert(std::abs(jittered.values[8] + 0.01F) < 0.00001F);
    assert(std::abs(jittered.values[9] - 0.02F) < 0.00001F);
    assert(std::abs(jittered.values[12] - 0.01F) < 0.00001F);
    assert(std::abs(jittered.values[13] + 0.02F) < 0.00001F);
    assert(jittered.values[10] == projection.values[10]);
    assert(jittered.values[14] == projection.values[14]);
    return 0;
}

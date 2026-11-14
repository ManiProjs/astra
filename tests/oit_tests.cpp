#include <astra/renderer/oit.hpp>

#include <cassert>
#include <cmath>

namespace {

bool close(float a, float b, float tolerance = 1.0e-5F) {
    return std::abs(a - b) <= tolerance;
}

} // namespace

int main() {
    using astra::renderer::WeightedOitAccumulator;
    using astra::rhi::Color;

    const Color opaque{0.08F, 0.12F, 0.2F, 1.0F};
    const Color red{0.95F, 0.08F, 0.04F, 1.0F};
    const Color blue{0.04F, 0.2F, 0.95F, 1.0F};

    WeightedOitAccumulator forward;
    forward.accumulate(red, 0.42F, 0.25F);
    forward.accumulate(blue, 0.63F, 0.72F);
    WeightedOitAccumulator reverse;
    reverse.accumulate(blue, 0.63F, 0.72F);
    reverse.accumulate(red, 0.42F, 0.25F);
    const auto a = forward.composite(opaque);
    const auto b = reverse.composite(opaque);
    assert(close(a.red, b.red));
    assert(close(a.green, b.green));
    assert(close(a.blue, b.blue));
    assert(a.alpha == opaque.alpha);

    WeightedOitAccumulator empty;
    const auto unchanged = empty.composite(opaque);
    assert(unchanged.red == opaque.red && unchanged.green == opaque.green &&
           unchanged.blue == opaque.blue && unchanged.alpha == opaque.alpha);

    WeightedOitAccumulator transparent;
    transparent.accumulate(red, 0.0F, 0.5F);
    const auto transparent_result = transparent.composite(opaque);
    assert(transparent_result.red == opaque.red && transparent_result.green == opaque.green &&
           transparent_result.blue == opaque.blue);
    return 0;
}

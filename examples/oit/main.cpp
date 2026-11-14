#include <astra/core/log.hpp>
#include <astra/game/game.hpp>

#include <array>
#include <cmath>
#include <utility>

namespace {

using astra::rhi::Matrix4;

Matrix4 multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 result{};
    result.values.fill(0.0F);
    for (std::size_t column = 0; column < 4; ++column)
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t k = 0; k < 4; ++k)
                result.values[column * 4 + row] +=
                    a.values[k * 4 + row] * b.values[column * 4 + k];
    return result;
}

Matrix4 translation(float x, float y, float z) {
    Matrix4 result{};
    result.values[12] = x;
    result.values[13] = y;
    result.values[14] = z;
    return result;
}

Matrix4 rotation_y(float angle) {
    Matrix4 result{};
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    result.values[0] = c;
    result.values[2] = -s;
    result.values[8] = s;
    result.values[10] = c;
    return result;
}

} // namespace

int main() {
    astra::core::Logger logger{astra::core::LogLevel::info};
    auto game_result = astra::game::Game::create({
        .window = {.title = "Astra - Weighted Blended OIT",
                   .width = 1100,
                   .height = 720,
                   .resizable = true,
                   .high_dpi = true},
    });
    if (!game_result) {
        logger.log(astra::core::LogLevel::critical, "Game", game_result.error().message);
        return 1;
    }
    auto game = std::move(*game_result);

    constexpr std::array<astra::rhi::MeshVertex, 8> vertices{{
        {{-1, -1, -1}, {-0.577F, -0.577F, -0.577F}, {1, 1, 1, 1}},
        {{1, -1, -1}, {0.577F, -0.577F, -0.577F}, {1, 1, 1, 1}},
        {{1, 1, -1}, {0.577F, 0.577F, -0.577F}, {1, 1, 1, 1}},
        {{-1, 1, -1}, {-0.577F, 0.577F, -0.577F}, {1, 1, 1, 1}},
        {{-1, -1, 1}, {-0.577F, -0.577F, 0.577F}, {1, 1, 1, 1}},
        {{1, -1, 1}, {0.577F, -0.577F, 0.577F}, {1, 1, 1, 1}},
        {{1, 1, 1}, {0.577F, 0.577F, 0.577F}, {1, 1, 1, 1}},
        {{-1, 1, 1}, {-0.577F, 0.577F, 0.577F}, {1, 1, 1, 1}},
    }};
    constexpr std::array<std::uint32_t, 36> indices{{
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
        0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2,
        1, 2, 6, 1, 6, 5, 0, 4, 7, 0, 7, 3,
    }};
    auto mesh_result = game.renderer().create_mesh({vertices, indices});
    if (!mesh_result) {
        logger.log(astra::core::LogLevel::critical, "Mesh", mesh_result.error().message);
        return 1;
    }
    auto mesh = std::move(*mesh_result);

    astra::rhi::MaterialData red_glass{};
    red_glass.base_color = {0.95F, 0.12F, 0.08F, 0.42F};
    red_glass.metallic = 0.0F;
    red_glass.roughness = 0.18F;
    red_glass.alpha_mode = 2U;
    astra::rhi::MaterialData blue_glass{};
    blue_glass.base_color = {0.08F, 0.28F, 1.0F, 0.58F};
    blue_glass.metallic = 0.0F;
    blue_glass.roughness = 0.12F;
    blue_glass.alpha_mode = 2U;

    logger.log(astra::core::LogLevel::info, "OIT",
               "Two intersecting translucent objects use weighted blended accumulation and revealage. "
               "Press Escape to exit.");
    game.run([&](astra::game::Frame& frame) {
        if (frame.input.pressed(astra::game::Key::escape)) {
            frame.window.request_close();
            return;
        }
        const float time = frame.elapsed_time;
        frame.camera.position = {4.5F, 2.8F, 6.5F};
        frame.camera.target = {0.0F, 0.0F, 0.0F};
        frame.draw(*mesh, multiply(translation(-0.65F, 0.0F, 0.0F), rotation_y(time * 0.48F)),
                   red_glass);
        frame.draw(*mesh, multiply(translation(0.65F, 0.0F, 0.0F), rotation_y(-time * 0.37F)),
                   blue_glass);
        astra::rhi::SceneSettings settings{};
        settings.time_seconds = time;
        if (auto result = frame.render({0.035F, 0.045F, 0.065F, 1.0F}, settings); !result) {
            logger.log(astra::core::LogLevel::error, "Renderer", result.error().message);
            game.close();
        }
    });
    return 0;
}

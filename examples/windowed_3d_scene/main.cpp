#include <astra/core/log.hpp>
#include <astra/game/game.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace {

using astra::rhi::Matrix4;

Matrix4 multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            result.values[column * 4 + row] = 0.0F;
            for (int k = 0; k < 4; ++k) {
                result.values[column * 4 + row] += a.values[k * 4 + row] * b.values[column * 4 + k];
            }
        }
    }
    return result;
}

Matrix4 rotation(float angle) {
    Matrix4 result{};
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    result.values[0] = c;
    result.values[2] = -s;
    result.values[8] = s;
    result.values[10] = c;
    return result;
}

Matrix4 translation(float x, float y, float z) {
    Matrix4 result{};
    result.values[12] = x;
    result.values[13] = y;
    result.values[14] = z;
    return result;
}

} // namespace

int main() {
    astra::core::Logger logger{astra::core::LogLevel::info};
    auto game_result = astra::game::Game::create({
        .window = {
            .title = "Astra - 3D Mesh + Depth + Lighting",
            .width = 1280,
            .height = 800,
            .resizable = true,
            .high_dpi = true,
        },
    });
    if (!game_result) {
        logger.log(astra::core::LogLevel::critical, "Game", game_result.error().message);
        return 1;
    }
    auto game = std::move(*game_result);

    constexpr std::array<astra::rhi::MeshVertex, 24> vertices{{
        // Front
        {{-1, -1, 1}, {0, 0, 1}, {0.92F, 0.24F, 0.16F, 1}},
        {{1, -1, 1}, {0, 0, 1}, {0.92F, 0.24F, 0.16F, 1}},
        {{1, 1, 1}, {0, 0, 1}, {0.92F, 0.24F, 0.16F, 1}},
        {{-1, 1, 1}, {0, 0, 1}, {0.92F, 0.24F, 0.16F, 1}},
        // Back
        {{1, -1, -1}, {0, 0, -1}, {0.18F, 0.55F, 0.95F, 1}},
        {{-1, -1, -1}, {0, 0, -1}, {0.18F, 0.55F, 0.95F, 1}},
        {{-1, 1, -1}, {0, 0, -1}, {0.18F, 0.55F, 0.95F, 1}},
        {{1, 1, -1}, {0, 0, -1}, {0.18F, 0.55F, 0.95F, 1}},
        // Right
        {{1, -1, 1}, {1, 0, 0}, {0.18F, 0.78F, 0.46F, 1}},
        {{1, -1, -1}, {1, 0, 0}, {0.18F, 0.78F, 0.46F, 1}},
        {{1, 1, -1}, {1, 0, 0}, {0.18F, 0.78F, 0.46F, 1}},
        {{1, 1, 1}, {1, 0, 0}, {0.18F, 0.78F, 0.46F, 1}},
        // Left
        {{-1, -1, -1}, {-1, 0, 0}, {0.95F, 0.68F, 0.18F, 1}},
        {{-1, -1, 1}, {-1, 0, 0}, {0.95F, 0.68F, 0.18F, 1}},
        {{-1, 1, 1}, {-1, 0, 0}, {0.95F, 0.68F, 0.18F, 1}},
        {{-1, 1, -1}, {-1, 0, 0}, {0.95F, 0.68F, 0.18F, 1}},
        // Top
        {{-1, 1, 1}, {0, 1, 0}, {0.72F, 0.32F, 0.88F, 1}},
        {{1, 1, 1}, {0, 1, 0}, {0.72F, 0.32F, 0.88F, 1}},
        {{1, 1, -1}, {0, 1, 0}, {0.72F, 0.32F, 0.88F, 1}},
        {{-1, 1, -1}, {0, 1, 0}, {0.72F, 0.32F, 0.88F, 1}},
        // Bottom
        {{-1, -1, -1}, {0, -1, 0}, {0.20F, 0.78F, 0.82F, 1}},
        {{1, -1, -1}, {0, -1, 0}, {0.20F, 0.78F, 0.82F, 1}},
        {{1, -1, 1}, {0, -1, 0}, {0.20F, 0.78F, 0.82F, 1}},
        {{-1, -1, 1}, {0, -1, 0}, {0.20F, 0.78F, 0.82F, 1}},
    }};
    constexpr std::array<std::uint32_t, 36> indices{{
        0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
        12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
    }};
    auto mesh_result = game.renderer().create_mesh({vertices, indices});
    if (!mesh_result) {
        logger.log(astra::core::LogLevel::critical, "Mesh", mesh_result.error().message);
        return 1;
    }
    auto mesh = std::move(*mesh_result);
    logger.log(astra::core::LogLevel::info, "Renderer",
               "Initialized GPU: " + game.renderer().device_name());
    logger.log(
        astra::core::LogLevel::info, "Scene",
        "Rendering two indexed, depth-tested, lit 3D objects in one scene. Press Escape to exit.");

    std::uint64_t frame_count = 0;
    game.run([&](astra::game::Frame& frame) {
        if (frame.input.pressed(astra::game::Key::escape)) {
            frame.window.request_close();
            return;
        }
        if (frame.window.width() == 0 || frame.window.height() == 0)
            return;

        const float time = frame.elapsed_time;
        frame.camera.position = {3.2F, 2.3F, 4.2F};
        frame.camera.target = {0.0F, 0.0F, 0.0F};
        const Matrix4 spin = rotation(time * 0.65F);
        frame.draw(*mesh, multiply(translation(-1.2F, 0.0F, 0.0F), spin));
        frame.draw(*mesh, multiply(translation(1.2F, 0.0F, 0.0F), rotation(-time * 0.45F)));
        if (auto result = frame.render({0.025F, 0.035F, 0.055F, 1.0F}); !result) {
            logger.log(astra::core::LogLevel::error, "Renderer", result.error().message);
            game.close();
            return;
        }
        ++frame_count;
    });
    logger.log(astra::core::LogLevel::info, "Renderer",
               "Presented " + std::to_string(frame_count) + " frames.");
    return 0;
}

#include <astra/core/log.hpp>
#include <astra/assets/hdr.hpp>
#include <astra/game/game.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <numbers>
#include <utility>
#include <vector>

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

Matrix4 scaling(float value) {
    Matrix4 result{};
    result.values[0] = value;
    result.values[5] = value;
    result.values[10] = value;
    return result;
}

std::vector<float> make_studio_environment(std::uint32_t width, std::uint32_t height) {
    std::vector<float> pixels(static_cast<std::size_t>(width) * height * 4U, 1.0F);
    const astra::rhi::Vec3 sun = {0.34F, 0.72F, 0.60F};
    for (std::uint32_t y = 0; y < height; ++y) {
        const float latitude = (0.5F - (static_cast<float>(y) + 0.5F) / height) *
                               std::numbers::pi_v<float>;
        const float cos_latitude = std::cos(latitude);
        for (std::uint32_t x = 0; x < width; ++x) {
            const float longitude = ((static_cast<float>(x) + 0.5F) / width - 0.5F) *
                                    (2.0F * std::numbers::pi_v<float>);
            const astra::rhi::Vec3 direction{
                cos_latitude * std::cos(longitude), std::sin(latitude),
                cos_latitude * std::sin(longitude)};
            const float sun_dot = std::max(0.0F, direction.x * sun.x +
                                                     direction.y * sun.y +
                                                     direction.z * sun.z);
            const float horizon = std::clamp(direction.y * 0.5F + 0.5F, 0.0F, 1.0F);
            const float ground = std::clamp(-direction.y * 3.0F, 0.0F, 1.0F);
            const float sun_disk = std::pow(sun_dot, 900.0F) * 110.0F;
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4U;
            pixels[offset + 0] = std::lerp(0.12F, 0.72F, horizon) * (1.0F - ground) +
                                 0.035F * ground + sun_disk;
            pixels[offset + 1] = std::lerp(0.18F, 0.82F, horizon) * (1.0F - ground) +
                                 0.045F * ground + sun_disk * 0.94F;
            pixels[offset + 2] = std::lerp(0.28F, 1.0F, horizon) * (1.0F - ground) +
                                 0.055F * ground + sun_disk * 0.82F;
            pixels[offset + 3] = 1.0F;
        }
    }
    return pixels;
}

std::pair<std::vector<astra::rhi::MeshVertex>, std::vector<std::uint32_t>> make_sphere() {
    constexpr std::uint32_t rings = 24;
    constexpr std::uint32_t segments = 48;
    std::vector<astra::rhi::MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve((rings + 1U) * (segments + 1U));
    indices.reserve(rings * segments * 6U);
    for (std::uint32_t y = 0; y <= rings; ++y) {
        const float v = static_cast<float>(y) / rings;
        const float latitude = (0.5F - v) * std::numbers::pi_v<float>;
        const float ring_radius = std::cos(latitude);
        for (std::uint32_t x = 0; x <= segments; ++x) {
            const float u = static_cast<float>(x) / segments;
            const float longitude = u * 2.0F * std::numbers::pi_v<float>;
            const astra::rhi::Vec3 normal{ring_radius * std::cos(longitude),
                                          std::sin(latitude),
                                          ring_radius * std::sin(longitude)};
            vertices.push_back({normal, normal, {1.0F, 1.0F, 1.0F, 1.0F}, u, v});
        }
    }
    for (std::uint32_t y = 0; y < rings; ++y) {
        for (std::uint32_t x = 0; x < segments; ++x) {
            const std::uint32_t a = y * (segments + 1U) + x;
            const std::uint32_t b = a + segments + 1U;
            indices.insert(indices.end(), {a, a + 1U, b, a + 1U, b + 1U, b});
        }
    }
    return {std::move(vertices), std::move(indices)};
}

} // namespace

int main(int argc, char** argv) {
    astra::core::Logger logger{astra::core::LogLevel::info};
    auto game_result = astra::game::Game::create({
        .window = {.title = "Astra - Full Image-Based Lighting",
                   .width = 1120,
                   .height = 760,
                   .resizable = true,
                   .high_dpi = true},
    });
    if (!game_result) {
        logger.log(astra::core::LogLevel::critical, "Game", game_result.error().message);
        return 1;
    }
    auto game = std::move(*game_result);

    std::uint32_t environment_width = 256;
    std::uint32_t environment_height = 128;
    std::vector<float> environment_pixels;
    if (argc > 1) {
        std::ifstream file{std::filesystem::path{argv[1]}, std::ios::binary | std::ios::ate};
        if (!file) {
            logger.log(astra::core::LogLevel::critical, "HDR", "Could not open the supplied .hdr file.");
            return 1;
        }
        const auto file_size = file.tellg();
        if (file_size <= 0 || file_size > std::streamoff{512} * 1024 * 1024) {
            logger.log(astra::core::LogLevel::critical, "HDR",
                       "The supplied .hdr file is empty or exceeds the 512 MiB input limit.");
            return 1;
        }
        std::vector<std::byte> encoded(static_cast<std::size_t>(file_size));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(encoded.data()),
                       static_cast<std::streamsize>(encoded.size()))) {
            logger.log(astra::core::LogLevel::critical, "HDR", "Could not read the supplied .hdr file.");
            return 1;
        }
        auto decoded = astra::assets::decode_hdr(encoded);
        if (!decoded) {
            logger.log(astra::core::LogLevel::critical, "HDR", decoded.error().message);
            return 1;
        }
        environment_width = decoded->width;
        environment_height = decoded->height;
        environment_pixels = std::move(decoded->rgba32f);
        logger.log(astra::core::LogLevel::info, "HDR",
                   "Loaded Radiance RGBE environment " + std::to_string(environment_width) + "x" +
                       std::to_string(environment_height) + ".");
    } else {
        environment_pixels = make_studio_environment(environment_width, environment_height);
    }
    const astra::rhi::EnvironmentData environment_data{
        .width = environment_width,
        .height = environment_height,
        .rgba32f = environment_pixels,
        .environment_resolution = 128,
        .irradiance_resolution = 16,
        .prefilter_resolution = 128,
        .prefilter_mip_count = 5,
        .brdf_lut_resolution = 128,
    };
    auto environment_result = game.renderer().create_environment(environment_data);
    if (!environment_result) {
        logger.log(astra::core::LogLevel::critical, "IBL", environment_result.error().message);
        return 1;
    }
    auto environment = std::move(*environment_result);
    environment_pixels.clear();
    environment_pixels.shrink_to_fit();

    auto [vertices, indices] = make_sphere();
    auto mesh_result = game.renderer().create_mesh({vertices, indices});
    if (!mesh_result) {
        logger.log(astra::core::LogLevel::critical, "Mesh", mesh_result.error().message);
        return 1;
    }
    auto sphere = std::move(*mesh_result);

    std::array<astra::rhi::MaterialData, 5> materials{};
    materials[0].base_color = {0.92F, 0.58F, 0.16F, 1.0F};
    materials[0].metallic = 1.0F;
    materials[0].roughness = 0.12F;
    materials[1].base_color = {0.92F, 0.58F, 0.16F, 1.0F};
    materials[1].metallic = 1.0F;
    materials[1].roughness = 0.48F;
    materials[2].base_color = {0.72F, 0.76F, 0.82F, 1.0F};
    materials[2].metallic = 0.0F;
    materials[2].roughness = 0.08F;
    materials[3].base_color = {0.72F, 0.76F, 0.82F, 1.0F};
    materials[3].metallic = 0.0F;
    materials[3].roughness = 0.78F;
    materials[4].base_color = {0.12F, 0.38F, 0.72F, 1.0F};
    materials[4].metallic = 0.72F;
    materials[4].roughness = 0.3F;

    logger.log(astra::core::LogLevel::info, "IBL",
               "Environment converted to a cubemap; diffuse irradiance, GGX prefilter mips, and "
               "the split-sum BRDF LUT are ready. Pass a Radiance .hdr path to load another environment. "
               "Press Escape to exit.");
    game.run([&](astra::game::Frame& frame) {
        if (frame.input.pressed(astra::game::Key::escape)) {
            frame.window.request_close();
            return;
        }
        const float time = frame.elapsed_time;
        frame.camera.position = {0.0F, 2.1F, 8.5F};
        frame.camera.target = {0.0F, 0.0F, 0.0F};
        for (std::size_t index = 0; index < materials.size(); ++index) {
            const float x = (static_cast<float>(index) - 2.0F) * 1.65F;
            const float bob = std::sin(time * 0.7F + static_cast<float>(index)) * 0.05F;
            frame.draw(*sphere,
                       multiply(translation(x, bob, 0.0F), scaling(0.72F)), materials[index]);
        }
        astra::rhi::SceneSettings settings{};
        settings.time_seconds = time;
        settings.environment = environment.get();
        if (auto result = frame.render({0.025F, 0.035F, 0.055F, 1.0F}, settings); !result) {
            logger.log(astra::core::LogLevel::error, "Renderer", result.error().message);
            game.close();
        }
    });
    return 0;
}

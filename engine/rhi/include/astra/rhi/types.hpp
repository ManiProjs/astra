#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace astra::rhi {

// Keep API generations distinct: projects can pin a backend explicitly while
// `automatic` remains free to choose the best compiled implementation.
enum class Backend : std::uint8_t {
    automatic,
    direct3d9,
    direct3d10,
    direct3d11,
    direct3d12,
    vulkan,
    opengl,
    metal,
};

[[nodiscard]] constexpr const char* backend_name(Backend backend) noexcept {
    switch (backend) {
    case Backend::automatic: return "Automatic";
    case Backend::direct3d9: return "Direct3D 9";
    case Backend::direct3d10: return "Direct3D 10";
    case Backend::direct3d11: return "Direct3D 11";
    case Backend::direct3d12: return "Direct3D 12";
    case Backend::vulkan: return "Vulkan";
    case Backend::opengl: return "OpenGL";
    case Backend::metal: return "Metal";
    }
    return "Unknown";
}

struct Extent2D final {
    std::uint32_t width{};
    std::uint32_t height{};
};

struct Offset2D final {
    std::int32_t x{};
    std::int32_t y{};
};

struct Color final {
    float red{};
    float green{};
    float blue{};
    float alpha{1.0F};
};

struct Vec2 final { float x{}; float y{}; };
struct Vec3 final { float x{}; float y{}; float z{}; };
struct Vec4 final { float x{}; float y{}; float z{}; float w{}; };

struct Matrix4 final {
    // Column-major, matching Metal and the engine's existing transform convention.
    std::array<float, 16> values{
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F,
    };
};

enum class ErrorCode : std::uint8_t {
    unsupported,
    invalid_argument,
    invalid_state,
    out_of_memory,
    device_lost,
    resource_creation_failed,
    pipeline_creation_failed,
    command_recording_failed,
    submission_failed,
};

struct Error final {
    ErrorCode code{ErrorCode::invalid_state};
    std::string message;
};

} // namespace astra::rhi

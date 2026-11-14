#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

namespace astra::platform {

enum class OperatingSystem : unsigned char {
    macos,
    windows,
    linux_os,
    unknown,
};

enum class Architecture : unsigned char {
    arm64,
    x86_64,
    x86,
    unknown,
};

struct SystemInfo final {
    OperatingSystem operating_system;
    Architecture architecture;
    std::uint32_t logical_processor_count;
    std::string_view operating_system_name;
    std::string_view architecture_name;
};

using MonotonicClock = std::chrono::steady_clock;
using MonotonicTime = MonotonicClock::time_point;

[[nodiscard]] SystemInfo system_info() noexcept;
[[nodiscard]] MonotonicTime monotonic_time() noexcept;

} // namespace astra::platform

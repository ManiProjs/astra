#include <astra/platform/system_info.hpp>

#include <thread>

namespace astra::platform {
namespace {

constexpr OperatingSystem current_operating_system() noexcept {
#if defined(__APPLE__) && defined(__MACH__)
    return OperatingSystem::macos;
#elif defined(_WIN32)
    return OperatingSystem::windows;
#elif defined(__linux__)
    return OperatingSystem::linux_os;
#else
    return OperatingSystem::unknown;
#endif
}

constexpr Architecture current_architecture() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    return Architecture::arm64;
#elif defined(__x86_64__) || defined(_M_X64)
    return Architecture::x86_64;
#elif defined(__i386__) || defined(_M_IX86)
    return Architecture::x86;
#else
    return Architecture::unknown;
#endif
}

constexpr std::string_view operating_system_name(OperatingSystem operating_system) noexcept {
    switch (operating_system) {
    case OperatingSystem::macos:
        return "macOS";
    case OperatingSystem::windows:
        return "Windows";
    case OperatingSystem::linux_os:
        return "Linux";
    case OperatingSystem::unknown:
        return "Unknown";
    }
    return "Unknown";
}

constexpr std::string_view architecture_name(Architecture architecture) noexcept {
    switch (architecture) {
    case Architecture::arm64:
        return "ARM64";
    case Architecture::x86_64:
        return "x86_64";
    case Architecture::x86:
        return "x86";
    case Architecture::unknown:
        return "Unknown";
    }
    return "Unknown";
}

} // namespace

SystemInfo system_info() noexcept {
    const auto operating_system = current_operating_system();
    const auto architecture = current_architecture();
    const auto processor_count = std::thread::hardware_concurrency();

    return {
        .operating_system = operating_system,
        .architecture = architecture,
        .logical_processor_count = processor_count == 0 ? 1U : processor_count,
        .operating_system_name = operating_system_name(operating_system),
        .architecture_name = architecture_name(architecture),
    };
}

MonotonicTime monotonic_time() noexcept {
    return MonotonicClock::now();
}

} // namespace astra::platform

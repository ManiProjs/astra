#pragma once

#include <atomic>
#include <string_view>

namespace astra::core {

enum class LogLevel : unsigned char {
    trace,
    debug,
    info,
    warning,
    error,
    critical,
    off,
};

[[nodiscard]] constexpr std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::trace:
        return "TRACE";
    case LogLevel::debug:
        return "DEBUG";
    case LogLevel::info:
        return "INFO";
    case LogLevel::warning:
        return "WARN";
    case LogLevel::error:
        return "ERROR";
    case LogLevel::critical:
        return "CRITICAL";
    case LogLevel::off:
        return "OFF";
    }
    return "UNKNOWN";
}

class Logger final {
  public:
    explicit Logger(LogLevel level = LogLevel::info) noexcept;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void set_level(LogLevel level) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;
    [[nodiscard]] bool is_enabled(LogLevel message_level) const noexcept;
    void log(LogLevel message_level, std::string_view category, std::string_view message) const;

  private:
    std::atomic<LogLevel> level_;
};

} // namespace astra::core

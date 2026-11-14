#include <astra/core/log.hpp>

#include <iostream>
#include <mutex>

namespace astra::core {
namespace {

std::mutex output_mutex;

} // namespace

Logger::Logger(LogLevel level) noexcept : level_(level) {}

void Logger::set_level(LogLevel level) noexcept {
    level_.store(level, std::memory_order_relaxed);
}

LogLevel Logger::level() const noexcept {
    return level_.load(std::memory_order_relaxed);
}

bool Logger::is_enabled(LogLevel message_level) const noexcept {
    const auto configured_level = level();
    return configured_level != LogLevel::off &&
           static_cast<unsigned char>(message_level) <=
               static_cast<unsigned char>(LogLevel::critical) &&
           static_cast<unsigned char>(message_level) >=
               static_cast<unsigned char>(configured_level);
}

void Logger::log(LogLevel message_level, std::string_view category,
                 std::string_view message) const {
    if (!is_enabled(message_level)) {
        return;
    }

    const std::lock_guard lock{output_mutex};
    auto& output = std::clog;
    output << '[' << to_string(message_level) << "] [" << category << "] " << message << '\n';
}

} // namespace astra::core

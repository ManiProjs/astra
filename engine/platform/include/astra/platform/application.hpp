#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <string>

namespace astra::platform {

enum class ApplicationState : unsigned char {
    created,
    initializing,
    initialized,
    running,
    stopping,
    stopped,
    failed,
};

struct ApplicationConfig final {
    std::string name{"Astra Application"};
};

struct FrameContext final {
    std::chrono::duration<double> delta_time{};
    std::uint64_t frame_index{};
};

struct ApplicationCallbacks final {
    std::function<void()> on_initialize;
    // Return false to end the run loop normally.
    std::function<bool(const FrameContext&)> on_update;
    std::function<void()> on_shutdown;
};

class Application final {
  public:
    explicit Application(ApplicationConfig config = {}, ApplicationCallbacks callbacks = {});
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    [[nodiscard]] bool initialize() noexcept;
    [[nodiscard]] bool run() noexcept;
    // Safe to call from another thread to ask the run loop to exit.
    void request_shutdown() noexcept;
    // initialize(), run(), and shutdown() belong to one application thread.
    // Call shutdown() after run() returns; callbacks must not call lifecycle methods.
    void shutdown() noexcept;

    [[nodiscard]] ApplicationState state() const noexcept;
    [[nodiscard]] const ApplicationConfig& config() const noexcept;
    [[nodiscard]] std::exception_ptr failure() const noexcept;

  private:
    void fail(std::exception_ptr exception) noexcept;

    ApplicationConfig config_;
    ApplicationCallbacks callbacks_;
    std::atomic<ApplicationState> state_{ApplicationState::created};
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> shutdown_started_{false};
    mutable std::mutex failure_mutex_;
    std::exception_ptr failure_;
};

} // namespace astra::platform

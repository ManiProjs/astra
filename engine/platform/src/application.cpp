#include <astra/platform/application.hpp>

#include <utility>

namespace astra::platform {

Application::Application(ApplicationConfig config, ApplicationCallbacks callbacks)
    : config_(std::move(config)), callbacks_(std::move(callbacks)) {}

Application::~Application() {
    shutdown();
}

bool Application::initialize() noexcept {
    auto expected = ApplicationState::created;
    if (!state_.compare_exchange_strong(expected, ApplicationState::initializing,
                                        std::memory_order_acq_rel)) {
        return false;
    }

    try {
        if (callbacks_.on_initialize) {
            callbacks_.on_initialize();
        }
        initialized_.store(true, std::memory_order_release);
        state_.store(ApplicationState::initialized, std::memory_order_release);
        return true;
    } catch (...) {
        fail(std::current_exception());
        return false;
    }
}

bool Application::run() noexcept {
    auto expected = ApplicationState::initialized;
    if (!state_.compare_exchange_strong(expected, ApplicationState::running,
                                        std::memory_order_acq_rel)) {
        return false;
    }

    const auto start = std::chrono::steady_clock::now();
    auto previous = start;
    std::uint64_t frame_index = 0;

    try {
        while (callbacks_.on_update && !shutdown_requested_.load(std::memory_order_acquire)) {
            const auto now = std::chrono::steady_clock::now();
            const FrameContext frame{
                .delta_time = now - previous,
                .frame_index = frame_index++,
            };
            previous = now;

            if (callbacks_.on_update && !callbacks_.on_update(frame)) {
                break;
            }
        }
    } catch (...) {
        fail(std::current_exception());
        return false;
    }

    auto running = ApplicationState::running;
    state_.compare_exchange_strong(running, ApplicationState::stopping, std::memory_order_acq_rel);
    return true;
}

void Application::request_shutdown() noexcept {
    shutdown_requested_.store(true, std::memory_order_release);
}

void Application::shutdown() noexcept {
    request_shutdown();
    if (shutdown_started_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    const auto current = state_.load(std::memory_order_acquire);
    if (current == ApplicationState::stopped) {
        return;
    }
    const bool already_failed = current == ApplicationState::failed;

    if (!initialized_.load(std::memory_order_acquire)) {
        if (!already_failed) {
            state_.store(ApplicationState::stopped, std::memory_order_release);
        }
        return;
    }

    state_.store(ApplicationState::stopping, std::memory_order_release);

    try {
        if (callbacks_.on_shutdown) {
            callbacks_.on_shutdown();
        }
        state_.store(already_failed ? ApplicationState::failed : ApplicationState::stopped,
                     std::memory_order_release);
    } catch (...) {
        fail(std::current_exception());
    }
}

ApplicationState Application::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

const ApplicationConfig& Application::config() const noexcept {
    return config_;
}

std::exception_ptr Application::failure() const noexcept {
    const std::lock_guard lock{failure_mutex_};
    return failure_;
}

void Application::fail(std::exception_ptr exception) noexcept {
    const std::lock_guard lock{failure_mutex_};
    failure_ = std::move(exception);
    state_.store(ApplicationState::failed, std::memory_order_release);
}

} // namespace astra::platform

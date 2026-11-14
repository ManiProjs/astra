#include <astra/core/allocator.hpp>
#include <astra/core/config.hpp>
#include <astra/core/log.hpp>
#include <astra/core/thread_pool.hpp>
#include <astra/platform/system_info.hpp>

#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void test_logger_thresholds() {
    astra::core::Logger logger;
    expect(logger.level() == astra::core::LogLevel::info, "logger defaults to INFO");
    expect(logger.is_enabled(astra::core::LogLevel::error), "ERROR passes INFO threshold");
    expect(!logger.is_enabled(astra::core::LogLevel::debug), "DEBUG is filtered at INFO");

    logger.set_level(astra::core::LogLevel::trace);
    expect(logger.is_enabled(astra::core::LogLevel::trace), "TRACE passes TRACE threshold");

    logger.set_level(astra::core::LogLevel::off);
    expect(!logger.is_enabled(astra::core::LogLevel::critical), "OFF disables all messages");

    std::ostringstream captured;
    auto* original_buffer = std::clog.rdbuf(captured.rdbuf());
    logger.set_level(astra::core::LogLevel::warning);
    logger.log(astra::core::LogLevel::info, "Test", "filtered");
    logger.log(astra::core::LogLevel::error, "Test", "visible");
    std::clog.rdbuf(original_buffer);
    expect(captured.str().find("visible") != std::string::npos, "logger writes enabled messages");
    expect(captured.str().find("filtered") == std::string::npos,
           "logger does not write filtered messages");
}

void test_system_info() {
    const auto info = astra::platform::system_info();
    expect(info.operating_system != astra::platform::OperatingSystem::unknown,
           "host operating system is recognized");
    expect(info.architecture != astra::platform::Architecture::unknown,
           "host architecture is recognized");
    expect(info.logical_processor_count > 0, "logical processor count is never zero");
    expect(!info.operating_system_name.empty(), "operating system name is available");
    expect(!info.architecture_name.empty(), "architecture name is available");
}

void test_monotonic_clock() {
    const auto before = astra::platform::monotonic_time();
    const auto after = astra::platform::monotonic_time();
    expect(after >= before, "monotonic clock never moves backwards");
}

void test_thread_pool() {
    astra::core::ThreadPool pool{2};
    expect(pool.worker_count() == 2, "thread pool creates requested worker count");

    auto value = pool.submit([](int left, int right) { return left + right; }, 19, 23);
    expect(value.get() == 42, "thread pool returns task result through future");

    auto move_only = pool.submit([value = std::make_unique<int>(7)] { return *value; });
    expect(move_only.get() == 7, "thread pool accepts move-only callables");

    auto failure = pool.submit([]() -> int { throw std::runtime_error("expected task failure"); });
    bool exception_propagated = false;
    try {
        static_cast<void>(failure.get());
    } catch (const std::runtime_error&) {
        exception_propagated = true;
    }
    expect(exception_propagated, "thread pool propagates task exceptions through futures");

    pool.shutdown();
    expect(pool.is_stopping(), "thread pool reports shutdown state");

    bool rejected = false;
    try {
        static_cast<void>(pool.submit([] {}));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "thread pool rejects submissions after shutdown");
}

void test_thread_pool_shutdown_modes() {
    {
        astra::core::ThreadPool pool{1};
        std::promise<void> started;
        std::promise<void> release;
        auto release_signal = release.get_future().share();
        auto first = pool.submit([&] {
            started.set_value();
            release_signal.wait();
        });
        started.get_future().wait();
        auto queued = pool.submit([] { return 99; });
        std::thread shutdown_thread([&] { pool.shutdown(astra::core::ShutdownMode::drain); });
        while (!pool.is_stopping()) {
            std::this_thread::yield();
        }
        release.set_value();
        shutdown_thread.join();
        first.get();
        expect(queued.get() == 99, "drain shutdown executes accepted queued tasks");
    }

    {
        astra::core::ThreadPool pool{1};
        std::promise<void> started;
        std::promise<void> release;
        auto release_signal = release.get_future().share();
        auto first = pool.submit([&] {
            started.set_value();
            release_signal.wait();
        });
        started.get_future().wait();
        auto queued = pool.submit([] { return 99; });
        std::thread shutdown_thread(
            [&] { pool.shutdown(astra::core::ShutdownMode::cancel_pending); });
        while (!pool.is_stopping()) {
            std::this_thread::yield();
        }
        release.set_value();
        shutdown_thread.join();
        first.get();
        bool cancelled = false;
        try {
            static_cast<void>(queued.get());
        } catch (const std::future_error&) {
            cancelled = true;
        }
        expect(cancelled, "cancel_pending breaks futures for discarded queued tasks");
    }
}

void test_allocator() {
    astra::core::DefaultAllocator allocator;
    astra::core::TrackingAllocator tracking_allocator{allocator};
    astra::core::UniqueAllocation allocation{tracking_allocator, 256, 128};
    expect(static_cast<bool>(allocation), "RAII allocation owns allocated memory");
    expect(allocation.size() == 256, "RAII allocation retains requested size");
    expect(allocation.alignment() == 128, "RAII allocation retains requested alignment");
    expect(reinterpret_cast<std::uintptr_t>(allocation.get()) % 128 == 0,
           "allocator returns requested alignment");
    auto stats = tracking_allocator.stats();
    expect(stats.bytes_in_use == 256 && stats.live_allocations == 1,
           "tracking allocator reports live bytes and allocations");
    expect(stats.peak_bytes_in_use == 256 && stats.total_allocations == 1,
           "tracking allocator reports peak and total allocations");

    auto moved = std::move(allocation);
    expect(!allocation && static_cast<bool>(moved), "allocation move transfers ownership");
    moved.reset();
    expect(!moved, "reset releases allocation");
    stats = tracking_allocator.stats();
    expect(stats.bytes_in_use == 0 && stats.live_allocations == 0,
           "tracking allocator updates counters after deallocation");

    bool rejected = false;
    try {
        astra::core::UniqueAllocation invalid{allocator, 16, 3};
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    expect(rejected, "allocator rejects non-power-of-two alignment");
}

void test_config_store() {
    astra::core::ConfigStore config;
    expect(config.set_default("renderer.vsync", true), "configuration inserts default");
    expect(!config.set_default("renderer.vsync", false), "existing default is preserved");
    config.set("renderer.width", std::int64_t{1920});
    config.set("renderer.scale", 1.0);
    config.set("application.name", std::string{"Astra"});

    expect(config.get<bool>("renderer.vsync").value_or(false), "boolean config value reads");
    expect(config.get<std::int64_t>("renderer.width").value_or(0) == 1920,
           "integer config value reads");
    expect(config.get<double>("renderer.scale").value_or(0.0) == 1.0,
           "floating config value reads");
    expect(config.get<std::string>("application.name").value_or("") == "Astra",
           "string config value reads");
    expect(!config.get<double>("renderer.width"), "config type mismatch returns empty optional");
    expect(config.contains("renderer.width"), "configuration key lookup works");
    expect(config.keys().size() == 4, "configuration lists stored keys");
    expect(config.erase("renderer.width"), "configuration erases key");
    expect(!config.contains("renderer.width"), "erased key is no longer present");
}

} // namespace

int main() {
    test_logger_thresholds();
    test_system_info();
    test_monotonic_clock();
    test_thread_pool();
    test_thread_pool_shutdown_modes();
    test_allocator();
    test_config_store();
    std::cout << "All runtime foundation tests passed.\n";
    return EXIT_SUCCESS;
}

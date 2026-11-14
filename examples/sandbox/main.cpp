#include <astra/core/allocator.hpp>
#include <astra/core/config.hpp>
#include <astra/core/log.hpp>
#include <astra/core/thread_pool.hpp>
#include <astra/platform/application.hpp>
#include <astra/platform/filesystem.hpp>
#include <astra/platform/system_info.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <string>
#include <system_error>
#include <vector>

namespace {

struct SandboxRuntime final {
    astra::core::Logger logger{astra::core::LogLevel::debug};
    astra::core::ConfigStore config;
    astra::core::DefaultAllocator system_allocator;
    astra::core::TrackingAllocator allocator{system_allocator};
    astra::core::ThreadPool jobs{2};
    std::vector<std::future<std::string>> background_jobs;
    std::uint64_t frames{};

    void initialize() {
        using namespace astra::core;

        (void)config.set_default("sandbox.max_frames", std::int64_t{4});
        (void)config.set_default("sandbox.log_frames", true);
        logger.log(LogLevel::info, "Sandbox", "Initializing Astra runtime sample");

        const auto system = astra::platform::system_info();
        logger.log(LogLevel::info, "System",
                   std::string{system.operating_system_name} + " / " +
                       std::string{system.architecture_name});

        astra::core::UniqueAllocation frame_scratch{allocator, 16 * 1024, 64};
        std::memset(frame_scratch.get(), 0, frame_scratch.size());
        logger.log(LogLevel::debug, "Memory", "Reserved 16 KiB of aligned scratch memory");

        background_jobs.push_back(
            jobs.submit([] { return std::string{"Asset index task completed"}; }));
        background_jobs.push_back(
            jobs.submit([] { return std::string{"Shader metadata task completed"}; }));

        write_session_file();
    }

    void write_session_file() {
        namespace fs = astra::platform::filesystem;
        const auto directory = std::filesystem::temp_directory_path() / "astra-sandbox" / "Saved";
        if (auto created = fs::create_directories(directory); !created) {
            throw std::system_error(created.error(), "create sandbox directory");
        }

        const auto path = directory / "session.txt";
        const auto text =
            std::string{"Astra sandbox session\n"}.append("This is a headless runtime example.\n");
        if (auto written = fs::write_text_file(path, text); !written) {
            throw std::system_error(written.error(), "write sandbox session");
        }
        logger.log(astra::core::LogLevel::info, "Filesystem",
                   "Wrote sample session to " + path.string());
    }

    bool update(const astra::platform::FrameContext& frame) {
        frames = frame.frame_index + 1;
        if (config.get_or<bool>("sandbox.log_frames", true)) {
            logger.log(astra::core::LogLevel::debug, "Frame",
                       "Frame " + std::to_string(frame.frame_index) + " (delta " +
                           std::to_string(frame.delta_time.count()) + " s)");
        }
        return frames <
               static_cast<std::uint64_t>(config.get_or<std::int64_t>("sandbox.max_frames", 4));
    }

    void shutdown() {
        using namespace astra::core;

        jobs.shutdown(ShutdownMode::drain);
        for (auto& job : background_jobs) {
            logger.log(LogLevel::info, "Jobs", job.get());
        }

        const auto memory = allocator.stats();
        logger.log(LogLevel::info, "Memory",
                   "Tracked peak: " + std::to_string(memory.peak_bytes_in_use) + " bytes");
        logger.log(LogLevel::info, "Sandbox",
                   "Finished after " + std::to_string(frames) + " frames");
    }
};

} // namespace

int main() {
    SandboxRuntime runtime;
    astra::platform::Application app(
        {.name = "Astra Sandbox"},
        {
            .on_initialize = [&] { runtime.initialize(); },
            .on_update =
                [&](const astra::platform::FrameContext& frame) { return runtime.update(frame); },
            .on_shutdown = [&] { runtime.shutdown(); },
        });

    if (!app.initialize()) {
        runtime.logger.log(astra::core::LogLevel::critical, "Sandbox",
                           "Application initialization failed");
        return 1;
    }
    if (!app.run()) {
        runtime.logger.log(astra::core::LogLevel::critical, "Sandbox", "Application update failed");
        app.shutdown();
        return 1;
    }

    app.shutdown();
    return app.state() == astra::platform::ApplicationState::stopped ? 0 : 1;
}

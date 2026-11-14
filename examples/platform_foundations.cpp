#include <astra/core/log.hpp>
#include <astra/platform/application.hpp>
#include <astra/platform/filesystem.hpp>
#include <astra/platform/system_info.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

int main() {
    using astra::core::LogLevel;
    astra::core::Logger logger{LogLevel::debug};

    const auto system = astra::platform::system_info();
    logger.log(LogLevel::info, "Platform",
               std::string{system.operating_system_name} + " / " +
                   std::string{system.architecture_name} + " / " +
                   std::to_string(system.logical_processor_count) + " logical processors");

    namespace fs = astra::platform::filesystem;
    const fs::Path output_directory =
        std::filesystem::temp_directory_path() / "astra-platform-example";
    if (auto created = fs::create_directories(output_directory); !created) {
        logger.log(LogLevel::error, "Filesystem", created.error().message());
        return 1;
    }

    const auto output_file = output_directory / "welcome.txt";
    constexpr std::string_view message = "Astra's portable filesystem API is working.\n";
    if (auto written = fs::write_text_file(output_file, message); !written) {
        logger.log(LogLevel::error, "Filesystem", written.error().message());
        return 1;
    }

    const auto contents = fs::read_text_file(output_file);
    if (!contents) {
        logger.log(LogLevel::error, "Filesystem", contents.error().message());
        return 1;
    }
    logger.log(LogLevel::info, "Filesystem", *contents);

    const auto entries = fs::list_directory(output_directory);
    if (!entries) {
        logger.log(LogLevel::error, "Filesystem", entries.error().message());
        return 1;
    }
    logger.log(LogLevel::info, "Filesystem",
               "Entries in sample directory: " + std::to_string(entries->size()));

    std::uint64_t frames_seen = 0;
    astra::platform::Application app(
        {.name = "Astra Platform Example"},
        {
            .on_initialize = [&] { logger.log(LogLevel::info, "Application", "Initialized"); },
            .on_update =
                [&](const astra::platform::FrameContext& frame) {
                    frames_seen = frame.frame_index + 1;
                    logger.log(LogLevel::debug, "Application",
                               "Frame " + std::to_string(frame.frame_index) + ", delta " +
                                   std::to_string(frame.delta_time.count()) + " seconds");
                    return frames_seen < 3;
                },
            .on_shutdown = [&] { logger.log(LogLevel::info, "Application", "Shutdown callback"); },
        });

    if (!app.initialize()) {
        logger.log(LogLevel::error, "Application", "Initialization failed");
        return 1;
    }
    const bool ran = app.run();
    app.shutdown();
    if (!ran || app.state() != astra::platform::ApplicationState::stopped) {
        logger.log(LogLevel::error, "Application", "Lifecycle did not finish successfully");
        return 1;
    }

    logger.log(LogLevel::info, "Application",
               "Completed " + std::to_string(frames_seen) + " headless frames");
    return 0;
}

#include <astra/platform/application.hpp>
#include <astra/platform/filesystem.hpp>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void test_filesystem() {
    namespace fs = astra::platform::filesystem;
    const auto root = std::filesystem::temp_directory_path() /
                      ("astra-runtime-test-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    const auto created = fs::create_directories(root / "nested");
    expect(created.has_value(), "create_directories succeeds");

    const std::string expected = "Astra filesystem test\n";
    const auto written = fs::write_text_file(root / "nested" / "sample.txt", expected);
    expect(written.has_value(), "write_text_file succeeds");

    const auto present = fs::exists(root / "nested" / "sample.txt");
    expect(present.has_value() && *present, "exists reports written file");

    const auto read = fs::read_text_file(root / "nested" / "sample.txt");
    expect(read.has_value() && *read == expected, "read_text_file round-trips content");

    const auto empty_written = fs::write_text_file(root / "nested" / "empty.txt", "");
    expect(empty_written.has_value(), "write_text_file supports empty files");
    const auto empty_read = fs::read_text_file(root / "nested" / "empty.txt");
    expect(empty_read.has_value() && empty_read->empty(), "read_text_file reads empty files");

    const std::byte binary_contents[]{std::byte{0}, std::byte{1}, std::byte{0xff}};
    const auto binary_written = fs::write_file(root / "nested" / "binary.dat", binary_contents);
    expect(binary_written.has_value(), "write_file supports binary data");
    const auto binary_read = fs::read_file(root / "nested" / "binary.dat");
    expect(binary_read.has_value() && binary_read->size() == 3 &&
               (*binary_read)[0] == std::byte{0} && (*binary_read)[2] == std::byte{0xff},
           "read_file preserves binary data");
    expect(!fs::read_file(root / "missing.bin"), "read_file reports missing files as errors");
    expect(!fs::list_directory(root / "missing-directory"),
           "list_directory reports missing paths as errors");

    const auto entries = fs::list_directory(root / "nested");
    expect(entries.has_value() && entries->size() == 3, "list_directory returns file entries");

    std::error_code error;
    std::filesystem::remove_all(root, error);
    expect(!error, "test directory cleanup succeeds");
}

void test_application_lifecycle() {
    int initialized = 0;
    int updated = 0;
    int shut_down = 0;

    astra::platform::Application* app_pointer = nullptr;
    astra::platform::Application app(
        {.name = "Lifecycle test"},
        {
            .on_initialize =
                [&] {
                    ++initialized;
                    expect(app_pointer->state() == astra::platform::ApplicationState::initializing,
                           "initialization callback runs in INITIALIZING state");
                },
            .on_update =
                [&](const astra::platform::FrameContext& frame) {
                    expect(frame.frame_index == static_cast<std::uint64_t>(updated),
                           "frame indices increase monotonically");
                    ++updated;
                    return updated < 3;
                },
            .on_shutdown = [&] { ++shut_down; },
        });
    app_pointer = &app;

    expect(app.state() == astra::platform::ApplicationState::created,
           "application starts in CREATED state");
    expect(app.initialize(), "application initializes once");
    expect(!app.initialize(), "application cannot initialize twice");
    expect(app.state() == astra::platform::ApplicationState::initialized,
           "application enters INITIALIZED state");
    expect(app.run(), "application run loop completes");
    expect(updated == 3, "application updates until callback requests exit");
    expect(app.state() == astra::platform::ApplicationState::stopping,
           "run loop enters STOPPING state");
    app.shutdown();
    expect(app.state() == astra::platform::ApplicationState::stopped,
           "application enters STOPPED state");
    expect(initialized == 1 && shut_down == 1, "lifecycle callbacks run exactly once");
}

void test_application_failure() {
    astra::platform::Application app(
        {}, {.on_initialize = [] { throw std::runtime_error("expected test failure"); }});

    expect(!app.initialize(), "initialization reports callback failure");
    expect(app.state() == astra::platform::ApplicationState::failed,
           "failed initialization enters FAILED state");
    expect(app.failure() != nullptr, "application preserves callback exception");
    app.shutdown();
    expect(app.state() == astra::platform::ApplicationState::failed,
           "shutdown preserves FAILED state after initialization failure");
}

void test_application_update_failure() {
    int shut_down = 0;
    astra::platform::Application app(
        {}, {
                .on_update = [](const astra::platform::FrameContext&) -> bool {
                    throw std::runtime_error("expected update failure");
                },
                .on_shutdown = [&] { ++shut_down; },
            });

    expect(app.initialize(), "application initializes before update failure");
    expect(!app.run(), "run reports update callback failure");
    expect(app.state() == astra::platform::ApplicationState::failed,
           "update callback failure enters FAILED state");
    app.shutdown();
    expect(shut_down == 1, "shutdown callback runs after update failure");
    expect(app.state() == astra::platform::ApplicationState::failed,
           "shutdown preserves FAILED state after update failure");
}

void test_application_shutdown_request() {
    int updated = 0;
    astra::platform::Application* app_pointer = nullptr;
    astra::platform::Application app({}, {.on_update = [&](const astra::platform::FrameContext&) {
                                         ++updated;
                                         app_pointer->request_shutdown();
                                         return true;
                                     }});
    app_pointer = &app;

    expect(app.initialize(), "application initializes before shutdown request");
    expect(app.run(), "run loop accepts shutdown request");
    expect(updated == 1, "shutdown request exits after current update callback");
    app.shutdown();
    expect(app.state() == astra::platform::ApplicationState::stopped,
           "requested shutdown completes lifecycle");
}

} // namespace

int main() {
    test_filesystem();
    test_application_lifecycle();
    test_application_failure();
    test_application_update_failure();
    test_application_shutdown_request();
    std::cout << "All Platform runtime tests passed.\n";
    return EXIT_SUCCESS;
}

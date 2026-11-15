#include <astra/platform/window.hpp>

#include <thread>
#include <type_traits>

static_assert(std::is_same_v<decltype(&astra::platform::Window::set_display_sync),
                             void (astra::platform::Window::*)(bool) noexcept>);

int main() {
    constexpr auto menu_event = astra::platform::make_menu_action_event(0xA57U);
    static_assert(menu_event.type == astra::platform::WindowEventType::menu_action);
    static_assert(menu_event.command_id == 0xA57U);

    auto worker_error = astra::platform::WindowErrorCode::creation_failed;
    bool worker_created_window = false;
    std::thread worker([&] {
        const auto result = astra::platform::Window::create();
        worker_created_window = result.has_value();
        if (!result) {
            worker_error = result.error().code;
        }
    });
    worker.join();

#if (defined(__APPLE__) && !defined(ASTRA_TEST_UNSUPPORTED_BACKEND)) || defined(ASTRA_TEST_GLFW)
    if (worker_created_window ||
        worker_error != astra::platform::WindowErrorCode::not_main_thread) {
        return 1;
    }
#else
    if (worker_created_window ||
        worker_error != astra::platform::WindowErrorCode::unsupported_platform) {
        return 1;
    }
#endif

    const auto result = astra::platform::Window::create({
        .title = "Invalid dimensions test",
        .width = 0,
        .height = 720,
    });
    if (result) {
        return 1;
    }

#if (defined(__APPLE__) && !defined(ASTRA_TEST_UNSUPPORTED_BACKEND)) || defined(ASTRA_TEST_GLFW)
    return result.error().code == astra::platform::WindowErrorCode::creation_failed ? 0 : 1;
#else
    if (result.error().code != astra::platform::WindowErrorCode::unsupported_platform)
        return 1;
    // Unsupported adapters cannot create a window, so there is no native menu
    // target or event queue to mutate. Menu command construction remains portable.
    return menu_event.type == astra::platform::WindowEventType::menu_action &&
                   menu_event.command_id == 0xA57U
               ? 0
               : 1;
#endif
}

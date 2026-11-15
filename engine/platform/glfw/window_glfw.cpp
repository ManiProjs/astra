#include <astra/platform/window.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <mutex>
#include <thread>
#include <utility>

namespace astra::platform {

namespace {

std::mutex glfw_mutex;
std::size_t glfw_users{};
const std::thread::id process_main_thread = std::this_thread::get_id();

bool acquire_glfw() {
    std::lock_guard lock{glfw_mutex};
    if (glfw_users == 0 && glfwInit() != GLFW_TRUE)
        return false;
    ++glfw_users;
    return true;
}

void release_glfw() noexcept {
    std::lock_guard lock{glfw_mutex};
    if (glfw_users > 0 && --glfw_users == 0)
        glfwTerminate();
}

std::uint32_t modifiers(int mods) noexcept {
    std::uint32_t result{};
    if ((mods & GLFW_MOD_SHIFT) != 0) result |= static_cast<std::uint32_t>(WindowModifier::shift);
    if ((mods & GLFW_MOD_CONTROL) != 0) result |= static_cast<std::uint32_t>(WindowModifier::control);
    if ((mods & GLFW_MOD_ALT) != 0) result |= static_cast<std::uint32_t>(WindowModifier::alt);
    if ((mods & GLFW_MOD_SUPER) != 0) result |= static_cast<std::uint32_t>(WindowModifier::command);
    if ((mods & GLFW_MOD_CAPS_LOCK) != 0) result |= static_cast<std::uint32_t>(WindowModifier::caps_lock);
    return result;
}

} // namespace

struct Window::Impl final {
    WindowConfig config;
    GLFWwindow* window{};
    bool open{true};
    bool close_event_sent{};
    std::uint32_t pixel_width{};
    std::uint32_t pixel_height{};
    std::vector<WindowEvent> events;

    ~Impl() {
        if (window)
            glfwDestroyWindow(window);
        release_glfw();
    }

    static Impl* from(GLFWwindow* window) noexcept {
        return static_cast<Impl*>(glfwGetWindowUserPointer(window));
    }

    static void on_close(GLFWwindow* window) {
        auto* self = from(window);
        if (!self) return;
        self->open = false;
        glfwSetWindowShouldClose(window, GLFW_FALSE);
    }

    static void on_resize(GLFWwindow* window, int width, int height) {
        auto* self = from(window);
        if (!self) return;
        int pixel_width{}, pixel_height{};
        glfwGetFramebufferSize(window, &pixel_width, &pixel_height);
        self->pixel_width = static_cast<std::uint32_t>(std::max(pixel_width, 0));
        self->pixel_height = static_cast<std::uint32_t>(std::max(pixel_height, 0));
        self->events.push_back({.type = WindowEventType::resized,
                                .width = self->pixel_width,
                                .height = self->pixel_height});
        (void)width;
        (void)height;
    }

    static void on_key(GLFWwindow* window, int key, int scancode, int action, int mods) {
        auto* self = from(window);
        const int native_code = scancode >= 0 && scancode <= 0xffff ? scancode : key;
        if (!self || native_code < 0 || native_code > 0xffff) return;
        if (action != GLFW_PRESS && action != GLFW_RELEASE && action != GLFW_REPEAT) return;
        self->events.push_back({.type = action == GLFW_RELEASE ? WindowEventType::key_up
                                                               : WindowEventType::key_down,
                                .key_code = static_cast<std::uint16_t>(native_code),
                                .repeat = action == GLFW_REPEAT,
                                .modifiers = modifiers(mods)});
    }

    static void on_mouse_button(GLFWwindow* window, int button, int action, int mods) {
        auto* self = from(window);
        if (!self || button < 0 || button > 0xff) return;
        double x{}, y{};
        glfwGetCursorPos(window, &x, &y);
        self->events.push_back({.type = action == GLFW_RELEASE ? WindowEventType::mouse_up
                                                              : WindowEventType::mouse_down,
                                .mouse_button = static_cast<std::uint8_t>(button),
                                .x = x, .y = y, .modifiers = modifiers(mods)});
    }

    static void on_cursor(GLFWwindow* window, double x, double y) {
        auto* self = from(window);
        if (self)
            self->events.push_back({.type = WindowEventType::mouse_moved, .x = x, .y = y});
    }

    static void on_scroll(GLFWwindow* window, double x, double y) {
        auto* self = from(window);
        if (self)
            self->events.push_back({.type = WindowEventType::mouse_scrolled,
                                    .delta_x = x, .delta_y = y});
    }
};

Window::Window(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Window::~Window() = default;
Window::Window(Window&&) noexcept = default;
Window& Window::operator=(Window&&) noexcept = default;

std::expected<Window, WindowError> Window::create(WindowConfig config) {
    if (std::this_thread::get_id() != process_main_thread)
        return std::unexpected(WindowError{.code = WindowErrorCode::not_main_thread,
                                           .message = "GLFW windows must be created on the main thread."});
    if (config.width == 0 || config.height == 0)
        return std::unexpected(WindowError{.code = WindowErrorCode::creation_failed,
                                           .message = "Window dimensions must be greater than zero."});
    if (!acquire_glfw())
        return std::unexpected(WindowError{.code = WindowErrorCode::creation_failed,
                                           .message = "GLFW could not initialize the native window system."});

    auto impl = std::make_unique<Impl>();
    impl->config = std::move(config);
    glfwDefaultWindowHints();
    if (impl->config.client_api == WindowClientAPI::vulkan) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    } else {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    }
    glfwWindowHint(GLFW_RESIZABLE, impl->config.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, impl->config.visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, impl->config.high_dpi ? GLFW_TRUE : GLFW_FALSE);
    impl->window = glfwCreateWindow(static_cast<int>(impl->config.width),
                                    static_cast<int>(impl->config.height),
                                    impl->config.title.c_str(), nullptr, nullptr);
    if (!impl->window)
        return std::unexpected(WindowError{.code = WindowErrorCode::creation_failed,
                                           .message = "GLFW could not create a native window."});

    glfwSetWindowUserPointer(impl->window, impl.get());
    glfwSetWindowCloseCallback(impl->window, Impl::on_close);
    glfwSetFramebufferSizeCallback(impl->window, Impl::on_resize);
    glfwSetKeyCallback(impl->window, Impl::on_key);
    glfwSetMouseButtonCallback(impl->window, Impl::on_mouse_button);
    glfwSetCursorPosCallback(impl->window, Impl::on_cursor);
    glfwSetScrollCallback(impl->window, Impl::on_scroll);
    int pixel_width{}, pixel_height{};
    glfwGetFramebufferSize(impl->window, &pixel_width, &pixel_height);
    impl->pixel_width = static_cast<std::uint32_t>(std::max(pixel_width, 0));
    impl->pixel_height = static_cast<std::uint32_t>(std::max(pixel_height, 0));
    if (impl->config.client_api == WindowClientAPI::opengl) {
        glfwMakeContextCurrent(impl->window);
        glfwSwapInterval(1);
    }
    return Window{std::move(impl)};
}

bool Window::poll_events() {
    if (!impl_ || (!impl_->open && impl_->close_event_sent)) return false;
    impl_->events.clear();
    glfwPollEvents();
    if (!impl_->open && !impl_->close_event_sent) {
        impl_->events.push_back({.type = WindowEventType::close_requested});
        impl_->close_event_sent = true;
        return true;
    }
    return impl_->open;
}

std::vector<WindowEvent> Window::take_events() {
    if (!impl_) return {};
    auto events = std::move(impl_->events);
    impl_->events.clear();
    return events;
}

void Window::request_close() noexcept {
    if (impl_ && impl_->window) Impl::on_close(impl_->window);
}

void Window::set_title(std::string title) {
    if (!impl_ || !impl_->window) return;
    impl_->config.title = std::move(title);
    glfwSetWindowTitle(impl_->window, impl_->config.title.c_str());
}

void Window::set_menu_items(std::span<const WindowMenuItem> items) { (void)items; }
void Window::set_display_sync(bool enabled) noexcept {
    if (!impl_ || !impl_->window || impl_->config.client_api != WindowClientAPI::opengl) return;
    glfwMakeContextCurrent(impl_->window);
    glfwSwapInterval(enabled ? 1 : 0);
}

bool Window::is_open() const noexcept { return impl_ && impl_->open; }
std::uint32_t Window::width() const noexcept { return impl_ ? impl_->pixel_width : 0; }
std::uint32_t Window::height() const noexcept { return impl_ ? impl_->pixel_height : 0; }

const WindowConfig& Window::config() const noexcept {
    static const WindowConfig empty{};
    return impl_ ? impl_->config : empty;
}

void* Window::native_render_surface() const noexcept {
    return impl_ ? static_cast<void*>(impl_->window) : nullptr;
}

} // namespace astra::platform

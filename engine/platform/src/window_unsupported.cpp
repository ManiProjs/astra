#include <astra/platform/window.hpp>

#include <memory>
#include <utility>

namespace astra::platform {

struct Window::Impl final {
    WindowConfig config;
};

Window::Window(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Window::~Window() = default;
Window::Window(Window&&) noexcept = default;
Window& Window::operator=(Window&&) noexcept = default;

std::expected<Window, WindowError> Window::create(WindowConfig config) {
    (void)config;
    return std::unexpected(WindowError{
        .code = WindowErrorCode::unsupported_platform,
        .message = "Native windowing is not implemented for this platform yet.",
    });
}

bool Window::poll_events() {
    return false;
}
std::vector<WindowEvent> Window::take_events() {
    return {};
}
void Window::request_close() noexcept {}
void Window::set_title(std::string title) {
    if (impl_)
        impl_->config.title = std::move(title);
}
void Window::set_menu_items(std::span<const WindowMenuItem> items) {
    (void)items;
}
void Window::set_display_sync(bool enabled) noexcept {
    (void)enabled;
}
bool Window::is_open() const noexcept {
    return false;
}
std::uint32_t Window::width() const noexcept {
    return 0;
}
std::uint32_t Window::height() const noexcept {
    return 0;
}
const WindowConfig& Window::config() const noexcept {
    static const WindowConfig empty{};
    return impl_ ? impl_->config : empty;
}
void* Window::native_render_surface() const noexcept {
    return nullptr;
}

} // namespace astra::platform

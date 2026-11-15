#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace astra::platform {

enum class WindowErrorCode : unsigned char {
    unsupported_platform,
    not_main_thread,
    creation_failed,
};

struct WindowError final {
    WindowErrorCode code{};
    std::string message;
};

// GLFW must know whether a window will own an OpenGL context or be used as a
// Vulkan presentation surface before the native window is created. AppKit
// always exposes its CAMetalLayer, which can be consumed by Metal or Vulkan.
enum class WindowClientAPI : unsigned char { opengl, vulkan };

struct WindowConfig final {
    std::string title{"Astra Window"};
    std::uint32_t width{1280};
    std::uint32_t height{720};
    bool resizable{true};
    bool high_dpi{true};
    WindowClientAPI client_api{WindowClientAPI::opengl};
    bool visible{true};
};

enum class WindowEventType : unsigned char {
    close_requested,
    resized,
    key_down,
    key_up,
    mouse_down,
    mouse_up,
    mouse_moved,
    mouse_scrolled,
    menu_action,
};

enum class WindowModifier : std::uint32_t {
    shift = 1U << 0U,
    control = 1U << 1U,
    alt = 1U << 2U,
    command = 1U << 3U,
    caps_lock = 1U << 4U,
};

[[nodiscard]] constexpr bool has_modifier(std::uint32_t mask, WindowModifier modifier) noexcept {
    return (mask & static_cast<std::uint32_t>(modifier)) != 0;
}

struct WindowEvent final {
    WindowEventType type{};
    std::uint32_t width{};
    std::uint32_t height{};
    // Native hardware key code (macOS virtual key code on the AppKit adapter).
    std::uint16_t key_code{};
    std::uint32_t command_id{};
    std::uint8_t mouse_button{};
    bool repeat{};
    // Mouse positions are in client-area points; scroll deltas use native event units.
    double x{};
    double y{};
    double delta_x{};
    double delta_y{};
    // Bitmask of WindowModifier values.
    std::uint32_t modifiers{};
};

[[nodiscard]] constexpr WindowEvent make_menu_action_event(std::uint32_t command_id) noexcept {
    return {.type = WindowEventType::menu_action, .command_id = command_id};
}

struct WindowMenuItem final {
    std::string label;
    std::uint32_t command_id{};
    bool separator{};
};

class Window final {
  public:
    [[nodiscard]] static std::expected<Window, WindowError> create(WindowConfig config = {});

    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) noexcept;
    Window& operator=(Window&&) noexcept;

    // Creation, event polling, and destruction belong to the UI thread.
    // Drains pending native events. After close, returns true once more so the
    // close_requested event can be consumed, then returns false.
    [[nodiscard]] bool poll_events();
    // Returns and clears events collected by the most recent poll_events() call.
    [[nodiscard]] std::vector<WindowEvent> take_events();
    void request_close() noexcept;
    void set_title(std::string title);
    void set_menu_items(std::span<const WindowMenuItem> items);
    // Controls native display-refresh synchronization for the presentation surface.
    // Unsupported adapters accept the request as a no-op.
    void set_display_sync(bool enabled) noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] const WindowConfig& config() const noexcept;

    // Opaque platform presentation surface for the selected RHI backend.
    // On macOS this is a CAMetalLayer*. Do not dereference it in portable code.
    [[nodiscard]] void* native_render_surface() const noexcept;

  private:
    struct Impl;
    explicit Window(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace astra::platform

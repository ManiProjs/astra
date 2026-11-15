#pragma once

#include <astra/platform/window.hpp>
#include <astra/renderer/renderer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <vector>

namespace astra::game {

enum class Key : std::uint8_t {
    a, b, c, d, e, f, g, h, i, j, k, l, m,
    n, o, p, q, r, s, t, u, v, w, x, y, z,
    number_0, number_1, number_2, number_3, number_4,
    number_5, number_6, number_7, number_8, number_9,
    space, escape, enter, tab, backspace,
    left_shift, right_shift, left_control, right_control,
    left_alt, right_alt, left_command, right_command,
    left, down, right, up,
    count,
};

enum class MouseButton : std::uint8_t { left, right, middle, count };

class Input final {
  public:
    [[nodiscard]] bool down(Key key) const noexcept;
    [[nodiscard]] bool pressed(Key key) const noexcept;
    [[nodiscard]] bool released(Key key) const noexcept;
    [[nodiscard]] bool down(MouseButton button) const noexcept;
    [[nodiscard]] bool pressed(MouseButton button) const noexcept;
    [[nodiscard]] bool released(MouseButton button) const noexcept;
    [[nodiscard]] float mouse_x() const noexcept { return mouse_x_; }
    [[nodiscard]] float mouse_y() const noexcept { return mouse_y_; }
    [[nodiscard]] float mouse_delta_x() const noexcept { return mouse_delta_x_; }
    [[nodiscard]] float mouse_delta_y() const noexcept { return mouse_delta_y_; }
    [[nodiscard]] float scroll_x() const noexcept { return scroll_x_; }
    [[nodiscard]] float scroll_y() const noexcept { return scroll_y_; }
    [[nodiscard]] bool menu_command_pressed(std::uint32_t command_id) const noexcept;

  private:
    friend class Game;
    void begin_frame() noexcept;
    void process(const platform::WindowEvent& event) noexcept;
    std::array<bool, static_cast<std::size_t>(Key::count)> keys_{};
    std::array<bool, static_cast<std::size_t>(Key::count)> pressed_keys_{};
    std::array<bool, static_cast<std::size_t>(Key::count)> released_keys_{};
    std::array<bool, static_cast<std::size_t>(MouseButton::count)> buttons_{};
    std::array<bool, static_cast<std::size_t>(MouseButton::count)> pressed_buttons_{};
    std::array<bool, static_cast<std::size_t>(MouseButton::count)> released_buttons_{};
    float mouse_x_{};
    float mouse_y_{};
    float mouse_delta_x_{};
    float mouse_delta_y_{};
    float scroll_x_{};
    float scroll_y_{};
    bool have_mouse_position_{};
    std::vector<std::uint32_t> menu_commands_;
};

struct Config final {
    platform::WindowConfig window{};
    renderer::RendererConfig renderer{};
};

struct Error final {
    std::string message;
};

// A small perspective camera with a sensible game-ready default. Matrices are
// generated internally so a basic game does not need its own math library.
struct Camera final {
    rhi::Vec3 position{0.0F, 2.0F, 5.0F};
    rhi::Vec3 target{0.0F, 0.0F, 0.0F};
    rhi::Vec3 up{0.0F, 1.0F, 0.0F};
    float vertical_fov_degrees{60.0F};
    float near_clip{0.1F};
    float far_clip{1000.0F};
};

struct Frame final {
    std::uint64_t index{};
    float delta_time{};
    float elapsed_time{};
    const Input& input;
    platform::Window& window;
    renderer::Renderer& renderer;
    Camera& camera;

    // Queue a mesh for this frame. Call render() once after updating the scene.
    void draw(const rhi::GpuMesh& mesh, rhi::Matrix4 model = {},
              rhi::MaterialData material = {});
    void draw(rhi::DrawItem item);
    [[nodiscard]] std::expected<void, rhi::RenderError>
    render(rhi::Color clear_color = {0.035F, 0.045F, 0.065F, 1.0F},
           rhi::SceneSettings settings = {});

  private:
    friend class Game;
    Frame(std::uint64_t frame_index, float frame_delta, float elapsed,
          const Input& frame_input, platform::Window& frame_window,
          renderer::Renderer& frame_renderer, Camera& frame_camera,
          std::vector<rhi::DrawItem>& queued_draw_items) noexcept;
    std::vector<rhi::DrawItem>& draw_items;
};

// Owns the native window and renderer, translates input into named keys, and
// provides a single update loop. Advanced users can still access Window/Renderer.
class Game final {
  public:
    using Update = std::function<void(Frame&)>;

    [[nodiscard]] static std::expected<Game, Error> create(Config config = {});

    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;
    Game(Game&&) noexcept = default;
    Game& operator=(Game&&) noexcept = default;

    // Calls update once per presented frame until the window closes.
    void run(const Update& update);
    void close() noexcept;
    [[nodiscard]] platform::Window& window() noexcept { return window_; }
    [[nodiscard]] renderer::Renderer& renderer() noexcept { return renderer_; }
    [[nodiscard]] Camera& camera() noexcept { return camera_; }

  private:
    Game(platform::Window window, renderer::Renderer renderer) noexcept;
    platform::Window window_;
    renderer::Renderer renderer_;
    Input input_;
    Camera camera_;
    std::vector<rhi::DrawItem> draw_items_;
};

} // namespace astra::game

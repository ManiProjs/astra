#include <astra/game/game.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <utility>

namespace astra::game {
namespace {

constexpr std::size_t index(Key key) noexcept { return static_cast<std::size_t>(key); }
constexpr std::size_t index(MouseButton button) noexcept {
    return static_cast<std::size_t>(button);
}

bool valid(Key key) noexcept { return index(key) < index(Key::count); }
bool valid(MouseButton button) noexcept { return index(button) < index(MouseButton::count); }

bool translate_key(std::uint16_t code, Key& key) noexcept {
    // macOS virtual key codes are hidden here; game code uses named keys.
    switch (code) {
    case 0: key = Key::a; return true;
    case 11: key = Key::b; return true;
    case 8: key = Key::c; return true;
    case 2: key = Key::d; return true;
    case 14: key = Key::e; return true;
    case 3: key = Key::f; return true;
    case 5: key = Key::g; return true;
    case 4: key = Key::h; return true;
    case 34: key = Key::i; return true;
    case 38: key = Key::j; return true;
    case 40: key = Key::k; return true;
    case 37: key = Key::l; return true;
    case 46: key = Key::m; return true;
    case 45: key = Key::n; return true;
    case 31: key = Key::o; return true;
    case 35: key = Key::p; return true;
    case 12: key = Key::q; return true;
    case 15: key = Key::r; return true;
    case 1: key = Key::s; return true;
    case 17: key = Key::t; return true;
    case 32: key = Key::u; return true;
    case 9: key = Key::v; return true;
    case 13: key = Key::w; return true;
    case 7: key = Key::x; return true;
    case 16: key = Key::y; return true;
    case 6: key = Key::z; return true;
    case 29: key = Key::number_0; return true;
    case 18: key = Key::number_1; return true;
    case 19: key = Key::number_2; return true;
    case 20: key = Key::number_3; return true;
    case 21: key = Key::number_4; return true;
    case 23: key = Key::number_5; return true;
    case 22: key = Key::number_6; return true;
    case 26: key = Key::number_7; return true;
    case 28: key = Key::number_8; return true;
    case 25: key = Key::number_9; return true;
    case 49: key = Key::space; return true;
    case 53: key = Key::escape; return true;
    case 36: key = Key::enter; return true;
    case 48: key = Key::tab; return true;
    case 51: key = Key::backspace; return true;
    case 56: key = Key::left_shift; return true;
    case 60: key = Key::right_shift; return true;
    case 59: key = Key::left_control; return true;
    case 62: key = Key::right_control; return true;
    case 58: key = Key::left_alt; return true;
    case 61: key = Key::right_alt; return true;
    case 55: key = Key::left_command; return true;
    case 54: key = Key::right_command; return true;
    case 123: key = Key::left; return true;
    case 125: key = Key::down; return true;
    case 124: key = Key::right; return true;
    case 126: key = Key::up; return true;
    default: return false;
    }
}

rhi::Vec3 subtract(rhi::Vec3 a, rhi::Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
rhi::Vec3 cross(rhi::Vec3 a, rhi::Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float dot(rhi::Vec3 a, rhi::Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
rhi::Vec3 normalize(rhi::Vec3 value) {
    const float magnitude = std::sqrt(dot(value, value));
    return magnitude > 1.0e-6F ? rhi::Vec3{value.x / magnitude, value.y / magnitude,
                                         value.z / magnitude}
                               : rhi::Vec3{0.0F, 0.0F, -1.0F};
}

rhi::Matrix4 camera_view_projection(const Camera& camera, float aspect) {
    const auto forward = normalize(subtract(camera.target, camera.position));
    auto right = normalize(cross(forward, camera.up));
    if (dot(right, right) < 0.5F)
        right = normalize(cross(forward, {0.0F, 0.0F, 1.0F}));
    const auto up = cross(right, forward);
    rhi::Matrix4 view{};
    view.values[0] = right.x;
    view.values[4] = right.y;
    view.values[8] = right.z;
    view.values[1] = up.x;
    view.values[5] = up.y;
    view.values[9] = up.z;
    view.values[2] = -forward.x;
    view.values[6] = -forward.y;
    view.values[10] = -forward.z;
    view.values[12] = -dot(right, camera.position);
    view.values[13] = -dot(up, camera.position);
    view.values[14] = dot(forward, camera.position);

    rhi::Matrix4 projection{};
    projection.values.fill(0.0F);
    const float fov = camera.vertical_fov_degrees * std::numbers::pi_v<float> / 180.0F;
    const float factor = 1.0F / std::tan(fov * 0.5F);
    projection.values[0] = factor / aspect;
    projection.values[5] = factor;
    projection.values[10] = camera.far_clip / (camera.near_clip - camera.far_clip);
    projection.values[11] = -1.0F;
    projection.values[14] = camera.near_clip * camera.far_clip /
                            (camera.near_clip - camera.far_clip);

    rhi::Matrix4 result{};
    result.values.fill(0.0F);
    for (std::size_t column = 0; column < 4; ++column)
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t k = 0; k < 4; ++k)
                result.values[column * 4 + row] +=
                    projection.values[k * 4 + row] * view.values[column * 4 + k];
    return result;
}

rhi::Matrix4 inverse_matrix(const rhi::Matrix4& matrix) {
    double augmented[4][8]{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column)
            augmented[row][column] = matrix.values[column * 4 + row];
        augmented[row][row + 4] = 1.0;
    }
    for (std::size_t column = 0; column < 4; ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < 4; ++row)
            if (std::abs(augmented[row][column]) > std::abs(augmented[pivot][column]))
                pivot = row;
        if (std::abs(augmented[pivot][column]) < 1.0e-12)
            return {};
        if (pivot != column)
            for (std::size_t k = 0; k < 8; ++k)
                std::swap(augmented[pivot][k], augmented[column][k]);
        const double divisor = augmented[column][column];
        for (double& value : augmented[column])
            value /= divisor;
        for (std::size_t row = 0; row < 4; ++row) {
            if (row == column)
                continue;
            const double factor = augmented[row][column];
            for (std::size_t k = 0; k < 8; ++k)
                augmented[row][k] -= factor * augmented[column][k];
        }
    }
    rhi::Matrix4 result{};
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            result.values[column * 4 + row] =
                static_cast<float>(augmented[row][column + 4]);
    return result;
}

} // namespace

Frame::Frame(std::uint64_t frame_index, float frame_delta, float elapsed,
             const Input& frame_input, platform::Window& frame_window,
             renderer::Renderer& frame_renderer, Camera& frame_camera,
             std::vector<rhi::DrawItem>& queued_draw_items) noexcept
    : index(frame_index), delta_time(frame_delta), elapsed_time(elapsed), input(frame_input),
      window(frame_window), renderer(frame_renderer), camera(frame_camera),
      draw_items(queued_draw_items) {}

void Frame::draw(const rhi::GpuMesh& mesh, rhi::Matrix4 model, rhi::MaterialData material) {
    rhi::DrawItem item{};
    item.mesh = &mesh;
    item.model = model;
    item.material = material;
    item.use_material = true;
    draw_items.push_back(item);
}

std::expected<void, rhi::RenderError> Frame::render(rhi::Color clear_color,
                                                    rhi::SceneSettings settings) {
    if (window.width() == 0U || window.height() == 0U ||
        !std::isfinite(camera.position.x) || !std::isfinite(camera.position.y) ||
        !std::isfinite(camera.position.z) || !std::isfinite(camera.target.x) ||
        !std::isfinite(camera.target.y) || !std::isfinite(camera.target.z) ||
        !std::isfinite(camera.vertical_fov_degrees) || camera.vertical_fov_degrees <= 1.0F ||
        camera.vertical_fov_degrees >= 179.0F || !std::isfinite(camera.near_clip) ||
        !std::isfinite(camera.far_clip) || camera.near_clip <= 0.0F ||
        camera.far_clip <= camera.near_clip) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::invalid_scene,
            .message = "Frame rendering requires a valid drawable and perspective camera."});
    }
    const float aspect = static_cast<float>(window.width()) / static_cast<float>(window.height());
    settings.camera_position = camera.position;
    const auto view_projection = camera_view_projection(camera, aspect);
    settings.inverse_view_projection = inverse_matrix(view_projection);
    return renderer.render_scene(draw_items, view_projection, clear_color, settings);
}

bool Input::down(Key key) const noexcept { return valid(key) && keys_[index(key)]; }
bool Input::pressed(Key key) const noexcept { return valid(key) && pressed_keys_[index(key)]; }
bool Input::released(Key key) const noexcept { return valid(key) && released_keys_[index(key)]; }
bool Input::down(MouseButton button) const noexcept {
    return valid(button) && buttons_[index(button)];
}
bool Input::pressed(MouseButton button) const noexcept {
    return valid(button) && pressed_buttons_[index(button)];
}
bool Input::released(MouseButton button) const noexcept {
    return valid(button) && released_buttons_[index(button)];
}
bool Input::menu_command_pressed(std::uint32_t command_id) const noexcept {
    return std::find(menu_commands_.begin(), menu_commands_.end(), command_id) !=
           menu_commands_.end();
}

void Input::begin_frame() noexcept {
    pressed_keys_.fill(false);
    released_keys_.fill(false);
    pressed_buttons_.fill(false);
    released_buttons_.fill(false);
    mouse_delta_x_ = 0.0F;
    mouse_delta_y_ = 0.0F;
    scroll_x_ = 0.0F;
    scroll_y_ = 0.0F;
    menu_commands_.clear();
}

void Input::process(const platform::WindowEvent& event) noexcept {
    if (event.type == platform::WindowEventType::menu_action) {
        menu_commands_.push_back(event.command_id);
        return;
    }
    if (event.type == platform::WindowEventType::key_down ||
        event.type == platform::WindowEventType::key_up) {
        Key key{};
        if (!translate_key(event.key_code, key))
            return;
        const auto slot = index(key);
        if (event.type == platform::WindowEventType::key_down) {
            if (!keys_[slot])
                pressed_keys_[slot] = true;
            keys_[slot] = true;
        } else {
            if (keys_[slot])
                released_keys_[slot] = true;
            keys_[slot] = false;
        }
        return;
    }
    if (event.type == platform::WindowEventType::mouse_down ||
        event.type == platform::WindowEventType::mouse_up) {
        const auto button = static_cast<MouseButton>(event.mouse_button);
        if (!valid(button))
            return;
        const auto slot = index(button);
        if (event.type == platform::WindowEventType::mouse_down) {
            if (!buttons_[slot])
                pressed_buttons_[slot] = true;
            buttons_[slot] = true;
            mouse_x_ = static_cast<float>(event.x);
            mouse_y_ = static_cast<float>(event.y);
            have_mouse_position_ = true;
        } else {
            if (buttons_[slot])
                released_buttons_[slot] = true;
            buttons_[slot] = false;
        }
        return;
    }
    if (event.type == platform::WindowEventType::mouse_moved) {
        if (have_mouse_position_) {
            mouse_delta_x_ += static_cast<float>(event.x) - mouse_x_;
            mouse_delta_y_ += static_cast<float>(event.y) - mouse_y_;
        }
        mouse_x_ = static_cast<float>(event.x);
        mouse_y_ = static_cast<float>(event.y);
        have_mouse_position_ = true;
    } else if (event.type == platform::WindowEventType::mouse_scrolled) {
        scroll_x_ += static_cast<float>(event.delta_x);
        scroll_y_ += static_cast<float>(event.delta_y);
    }
}

Game::Game(platform::Window window, renderer::Renderer renderer) noexcept
    : window_(std::move(window)), renderer_(std::move(renderer)) {
    draw_items_.reserve(128);
}

std::expected<Game, Error> Game::create(Config config) {
    auto window = platform::Window::create(std::move(config.window));
    if (!window)
        return std::unexpected(Error{window.error().message});
    auto renderer = renderer::Renderer::create(*window, config.renderer);
    if (!renderer)
        return std::unexpected(Error{renderer.error().message});
    return Game{std::move(*window), std::move(*renderer)};
}

void Game::run(const Update& update) {
    if (!update)
        return;
    const auto start = std::chrono::steady_clock::now();
    auto previous = start;
    std::uint64_t frame_index = 0;
    while (window_.poll_events()) {
        input_.begin_frame();
        for (const auto& event : window_.take_events()) {
            input_.process(event);
            if (event.type == platform::WindowEventType::close_requested)
                return;
        }
        if (!window_.is_open())
            break;
        const auto now = std::chrono::steady_clock::now();
        const float delta_time = std::clamp(std::chrono::duration<float>(now - previous).count(),
                                            0.0F, 0.1F);
        previous = now;
        draw_items_.clear();
        Frame frame{frame_index++, delta_time,
                    std::chrono::duration<float>(now - start).count(),
                    input_, window_, renderer_, camera_, draw_items_};
        update(frame);
    }
}

void Game::close() noexcept { window_.request_close(); }

} // namespace astra::game

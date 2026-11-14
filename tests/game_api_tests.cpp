#include <astra/game/game.hpp>

#include <cassert>
#include <type_traits>

int main() {
    static_assert(std::is_move_constructible_v<astra::game::Game>);
    static_assert(!std::is_copy_constructible_v<astra::game::Game>);

    const astra::game::Input input;
    assert(!input.down(astra::game::Key::w));
    assert(!input.pressed(astra::game::Key::space));
    assert(!input.released(astra::game::Key::escape));
    assert(!input.down(astra::game::MouseButton::left));
    assert(input.mouse_delta_x() == 0.0F);
    assert(input.mouse_delta_y() == 0.0F);
    assert(input.scroll_x() == 0.0F);
    assert(input.scroll_y() == 0.0F);
    assert(!input.menu_command_pressed(1));
    return 0;
}

# Your first Astra game

The game-facing API owns the native window and renderer, pumps events, tracks named
keyboard/mouse input, and supplies frame timing. You can still use the lower-level
Platform and Renderer APIs directly when you need more control.

## A visible 3D game in a few lines

```cpp
#include <astra/game/game.hpp>
#include <array>
#include <utility>

int main() {
    auto created = astra::game::Game::create({
        .window = {
            .title = "My First Game",
            .width = 1280,
            .height = 720,
        },
    });
    if (!created)
        return 1;
    auto game = std::move(*created);

    constexpr std::array<astra::rhi::MeshVertex, 3> vertices{{
        {{-1, -1, 0}, {0, 0, 1}, {1, 0.2F, 0.1F, 1}},
        {{ 1, -1, 0}, {0, 0, 1}, {0.1F, 0.8F, 0.3F, 1}},
        {{ 0,  1, 0}, {0, 0, 1}, {0.2F, 0.4F, 1, 1}},
    }};
    constexpr std::array<std::uint32_t, 3> indices{0, 1, 2};
    auto created_mesh = game.renderer().create_mesh({vertices, indices});
    if (!created_mesh)
        return 1;
    auto mesh = std::move(*created_mesh);

    game.run([&](astra::game::Frame& frame) {
        if (frame.input.pressed(astra::game::Key::escape))
            return frame.window.request_close();
        frame.draw(*mesh);
        if (auto result = frame.render(); !result)
            frame.window.request_close();
    });
}
```

`Frame` provides `delta_time` (seconds, capped at 0.1), `elapsed_time`, `index`, named
keyboard/mouse `down`, `pressed`, and `released` states, mouse movement/scroll, and references
to the owned `window`, `renderer`, and default `camera`. `frame.draw(mesh, transform, material)` queues
a mesh; `frame.render()` builds the perspective camera matrices and submits the scene. Use
`frame.camera.position` and `frame.camera.target` to move the camera. Advanced games can bypass this
convenience path and call `frame.renderer.render_scene(...)` directly.

The initial native input adapter maps common keys to macOS virtual key codes internally.
Game code should use `astra::game::Key`, not platform-specific numeric codes. Text input, IME,
gamepads, a general scene/entity system, and asset/material authoring helpers are not implemented yet.

## Build

```sh
cmake --preset macos-arm64-debug
cmake --build --preset macos-arm64-debug --target astra_windowed_3d_scene
./.cache/build/macos-arm64-debug/astra_windowed_3d_scene
```

See `examples/windowed_3d_scene/main.cpp` for a complete example using `Game` with a mesh,
camera matrices, and a render submission.

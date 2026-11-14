# Astra

Astra is an experimental modular C++23 3D game engine. Its macOS vertical slice combines a Metal
PBR renderer, imported glTF characters and skeletal animation, texture-backed materials, cascaded
shadows, HDR/TAA, procedural sky/clouds/fog, heightfield character physics, GPU-driven terrain
cluster LOD, and asynchronous open-world streaming. It is an early custom engine, not an Unreal
Engine replacement; the renderer and simulation still have important documented limitations.

## Platform strategy

Astra is being structured for macOS, Windows, and Linux. Apple Silicon macOS is the initial
development host, while Windows and Linux are first-class planned build targets. The native
engine uses C++23 and LLVM Clang-family compilers on every platform; Windows uses `clang-cl`
with the Windows SDK and linker from Visual Studio Build Tools. Rust 2024 supports portable
developer tools and asset infrastructure.

The platform layer owns operating-system integration, and the Rendering Hardware Interface
(RHI) separates the renderer from graphics APIs. The first implemented vertical slice is macOS
AppKit windowing with keyboard/mouse/window events and a Metal renderer. Vulkan, Direct3D 12,
and native Windows/Linux window adapters remain unimplemented.

Apple's Metal SDK comes from Xcode or Command Line Tools. Windows SDK components come from
Visual Studio Build Tools. No platform SDK or third-party engine dependency is vendored here.

The initial machine has 8 GB of unified memory. Build and tooling choices should remain
lightweight, and future runtime budgets must account for CPU and GPU allocations sharing the
same physical memory.

## Configure and build

Install CMake 3.25 or newer, Ninja, LLVM Clang, and the Rust toolchain (rustup with stable is
recommended). Platform SDK requirements are listed in
[docs/build-and-development.md](docs/build-and-development.md).

On the current Apple Silicon Mac, configure a Debug build:

```sh
cmake --preset debug
cmake --build --preset debug
```

Configure a Release build:

```sh
cmake --preset release
cmake --build --preset release
```

Windows and Linux have dedicated windows-x64-* and linux-x64-* presets, available on their
respective hosts. CMake builds Core, Platform, RHI, Renderer, Game, Assets, Geometry, World, and (on macOS) Metal targets,
runtime tests, and five runnable examples on macOS (three portable examples on Windows/Linux).
The native window, renderer, and open-world examples currently require macOS.

On Windows, use `cmake --preset windows-x64-debug` and
`cmake --build --preset windows-x64-debug`. On Linux, use
`cmake --preset linux-x64-debug` and `cmake --build --preset linux-x64-debug`.

## Run the examples

The runtime examples demonstrate the current public APIs:

~~~sh
./.cache/build/debug/astra_example_core
./.cache/build/debug/astra_example_platform
./.cache/build/debug/astra_sandbox
~~~

On macOS, launch the native window and Metal 3D scene with:

~~~sh
./.cache/build/debug/astra_windowed_3d_scene
~~~

The `astra::game::Game` facade is the recommended starting point for a new native game. It owns
the window and renderer, pumps events, and provides named keyboard/mouse input and frame timing,
while keeping the lower-level APIs available. Start with [Your first Astra game](docs/first-game.md).

To explore the procedural open-world walking demo:

~~~sh
./.cache/build/debug/astra_open_world
~~~

The open-world sample loads AstraPlayer and CC0 nature/terrain assets. Asset sources and motion-
capture attribution are documented beside the assets in `assets/`.

See [examples/README.md](examples/README.md) for details.

Validate the Rust workspace metadata:

```sh
cargo metadata --no-deps --format-version 1
```

The Rust workspace has no members until the first developer-tool crate is introduced, so
`cargo check` and `cargo fmt` have no targets to process yet. Run them after adding a crate.

## Repository map

See [docs/architecture.md](docs/architecture.md) for module ownership and dependency rules,
[docs/runtime-foundation.md](docs/runtime-foundation.md) for the initial runtime APIs, and
[docs/build-and-development.md](docs/build-and-development.md) for the local workflow.
See [CONTRIBUTING.md](CONTRIBUTING.md) before proposing changes.

## Installation size policy

Installed-size targets and accounting rules are defined in
[docs/installation-and-packaging.md](docs/installation-and-packaging.md). They are design
constraints for the future distribution system; no packages or package manager exist yet.

## License

Astra is licensed under the MIT License. See [LICENSE](LICENSE).

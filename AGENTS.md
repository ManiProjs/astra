# Astra Engine Development

## Workflow & Commands

- **Build:**
  - Debug: `cmake --preset debug && cmake --build --preset debug`
  - Release: `cmake --preset release && cmake --build --preset release`
  - Windows/Linux: Use `windows-x64-*` or `linux-x64-*` presets.

- **Run Examples:**
  - All built binaries are in `./.cache/build/<config>/`.
  - Native window/Metal example: `./.cache/build/debug/astra_windowed_3d_scene`
  - Open-world demo: `./.cache/build/debug/astra_open_world`

- **Important:** Always check `examples/` when adding features. If functionality changes, update the examples to ensure they remain functional and represent the latest API usage.

## Architecture Notes

- **Language:** C++23.
- **Compiler:** Requires LLVM Clang-family (AppleClang on macOS, `clang-cl` on Windows).
- **Tooling:** CMake + Ninja.
- **Shaders:** Metal backend (`backends/metal/shaders/*.metal`).
- **Dependencies:** No vendored SDKs.

## Operational Gotchas

- **Memory:** Shared CPU/GPU memory on Apple Silicon (8GB limit). Keep allocations lightweight.
- **Metal Shaders:** If adding/modifying uniforms, ensure `sizeof` and `offsetof` in `backends/metal/src/metal_device.mm` are updated and checked against corresponding `struct` definitions in `scene.metal`.
- **Validation:** Run `cargo` tools (`cargo check`, `cargo fmt`) if adding Rust components.

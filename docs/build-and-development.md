# Build and development environment

## Required tools

| Tool | Purpose | Minimum / selection |
| --- | --- | --- |
| macOS | Initial development host | Apple Silicon ARM64 preset; Apple Clang from Xcode or Command Line Tools |
| Windows | Planned desktop build host | LLVM `clang-cl`, Ninja, Windows SDK and linker from Visual Studio Build Tools |
| Linux | Planned desktop build host | LLVM Clang, Ninja, and the distribution's development headers and linker tools |
| CMake | Native project configuration | 3.25+ |
| Ninja | Native build generator | Current stable |
| Rust and Cargo | Supporting tools workspace | Rust 1.85+; stable recommended |
| clang-format | C++ formatting | LLVM version compatible with the installed Clang |
| clang-tidy | C++ static analysis | LLVM version compatible with the installed Clang |

No external C++ libraries are required. CMake does not fetch dependencies. The open-world sample
includes the attributed Cesium Man GLB asset under `assets/characters/`; see its license README.
Platform SDKs are
supplied by their respective development environments and must not be copied into the repository
or Astra installation. The macOS window adapter links AppKit and QuartzCore, and the Metal backend
links Metal and QuartzCore from the selected Xcode/Command Line Tools SDK. Windows development
requires the Windows SDK and linker tools from Visual Studio Build Tools. Linux development uses
the host distribution system headers and toolchain components.

## Configure and build

From the repository root:

```sh
cmake --preset debug
cmake --build --preset debug
```

Use `release` instead of `debug` for an optimized configuration. Build trees are stored in
`.cache/build/` and are ignored by Git. The macOS presets select Ninja, Apple Clang, ARM64, and
a macOS 14.0 deployment target. Windows presets select Ninja and LLVM `clang-cl`; Linux presets
select Ninja and LLVM Clang. Windows and Linux presets are shown when CMake runs on those hosts.

On Windows, run CMake from a Visual Studio Developer PowerShell or Developer Command Prompt so
`clang-cl`, the Windows SDK, and the Microsoft linker are discoverable. The C++ compiler remains
LLVM-based; the Microsoft linker and SDK are platform toolchain components.

On Linux, install Clang, Ninja, CMake, and the development packages required by the host
distribution. The Linux presets target the host's native architecture.

The runtime targets include Core, Platform, RHI, Renderer, Assets, Geometry, and World. On macOS,
AstraMetal adds the native Metal backend; CMake compiles the `.metal` shader sources into a
`.metallib` as part of the build. The asset image decoder uses Apple's ImageIO and CoreGraphics
frameworks. CTest covers allocation, configuration, logging, jobs, filesystem operations,
application lifecycle, window API validation, world streaming/character movement, virtual geometry,
animation sampling, glTF validation, and the included Cesium Man GLB. The interactive Metal 3D scene
and open-world walking samples are built as `astra_windowed_3d_scene` and `astra_open_world` and must
be run manually.
Windows and Linux presets are defined
but cannot be executed on this macOS development machine; they need validation on their respective
CI or developer hosts before those platforms are considered verified.

## Rust workspace

The root `Cargo.toml` is a resolver-3 workspace using Rust 2024. It intentionally has no
members yet. Add a crate under `rust/` when there is a concrete tool to implement; keep shared
dependency versions and workspace-wide lint policy at the root as the workspace grows.

```sh
cargo metadata --no-deps --format-version 1
```

The Rust workspace has no members yet. `cargo check --workspace` and `cargo fmt --all -- --check`
become applicable after the first developer-tool crate is added.

## Quality and CI direction

Use `clang-format` with `.clang-format`, `rustfmt` with `rustfmt.toml`, and `clang-tidy` for
static analysis when C++ targets exist. These LLVM utilities may need to be installed separately
if they are not included in the selected developer tools. Add unit and integration tests
alongside the module they validate, with cross-module tests under `tests/`. CI can validate CMake
configuration, Cargo metadata, formatting, and tests; add platform runners as backend support
expands. There is no CI workflow yet.

## Apple toolchain checks

```sh
xcode-select -p
xcrun --find clang++
xcrun --find metal
xcrun --show-sdk-path
```

The selected developer directory determines which Apple SDK and Metal compiler are used.

## Platform and graphics API boundaries

The build system has a working native window and Metal path on macOS. Windows and Linux currently
build an explicit unsupported-window adapter; Win32, Wayland, X11, Vulkan, and Direct3D 12 runtime
backends are still future implementations. macOS Vulkan support would require a portability layer such
as MoltenVK. These backend-specific requirements will be checked by their own CMake modules
when those backends are implemented; the shared engine must not require every SDK to be
installed on every host.

# Contributing to Astra

## Current project stage

Astra is at the repository-foundation stage. Keep changes focused on infrastructure,
architecture documentation, and carefully scoped groundwork until a system has an agreed
design. Do not add empty classes or stub implementations to make planned systems appear
complete.

## Development rules

- Use C++23 and LLVM Clang-family compilers for native engine code on every supported host.
  Use Apple Clang on macOS, `clang-cl` on Windows, and Clang on Linux. Do not use GCC.
- Use Rust 2024 for developer tools and supporting infrastructure.
- Keep third-party dependencies out until a concrete requirement justifies them.
- Keep generated output under `.cache/` or Cargo's `target/` directory.
- Preserve the module boundaries and dependency direction in `docs/architecture.md`.
- Keep operating-system APIs inside their matching platform implementation. Keep graphics API
  calls inside backend implementations behind the RHI.
- Do not vendor platform SDKs. Use Xcode/Command Line Tools on Apple platforms and the Windows
  SDK supplied by Visual Studio Build Tools on Windows.
- Treat the 8 GB unified-memory development machine as a real constraint when choosing tools.
- Do not commit generated assets, build output, local IDE state, or downloaded SDKs.

## Before submitting a change

Run the checks relevant to the files you changed:

```sh
cmake --preset debug
cmake --build --preset debug
cargo metadata --no-deps --format-version 1
```

Once a Rust crate exists, also run `cargo check --workspace` and `cargo fmt --all -- --check`.
Format C++ with `clang-format` using the root `.clang-format` file. Keep documentation current
when directory ownership, build steps, or installation policy changes.

## Changes that need an architecture decision

New runtime modules, graphics backends, external dependencies, package formats, and changes
to the installation-size budget should include a short design note explaining ownership,
dependencies, memory impact, and validation strategy.

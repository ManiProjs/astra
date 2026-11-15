# Installation and packaging policy

The engine is intended to be distributed as a modular installation. These figures are
installed-size budgets, not compressed download sizes.

| Profile | Target installed size |
| --- | ---: |
| Minimal | 3–5 GB |
| Standard | 10–15 GB |
| Complete | 15–20 GB |

**Hard cap:** a Standard or Complete installation must not exceed 20 GB under the accounting
rules below. The Standard profile is the normal recommended installation. Optional modules
and content can be installed separately.

## Accounting rules

- Measure the logical installed size of all files owned by the selected Astra profile after
  extraction and installation. Do not use compressed archive size.
- Include the engine runtime, editor, bundled developer tools, required runtime libraries,
  profile-selected templates, and all mandatory profile packages.
- Include package data, shaders, metadata, and support files installed by Astra.
- Exclude the operating system, Xcode, Command Line Tools, Apple SDKs (including Metal), GPU
  drivers, and other platform components supplied and maintained by the operating system.
- Exclude user projects, imported game assets, build intermediates, shader caches, logs, and
  user-generated content. Report these separately when measuring disk use.
- Count shared files once when profiles share the same installed file. Count separately
  installed copies separately.
- Keep optional downloads outside a profile's required-size total until selected for install.
- Publish a machine-readable package manifest with each release so profile totals can be
  reproduced. The installer must reject a Standard or Complete plan whose computed installed
  total exceeds 20 GB.

## Future package manager requirements

The package manager is planned to support optional components, dependency resolution, package
integrity verification, signed metadata, stable/beta/development channels, incremental and
transactional updates, rollback, and cache management. Package metadata should declare
installed-size contributions and dependencies so the installer can calculate profile totals
before changing the installation.

No package manager, package format, installer, or downloadable content is implemented in this
repository yet.

## Optional Astra Learning component

The editor does not embed examples or learning guides. They are distributed as the independently
selectable `AstraLearning` CMake install component, so minimal editor/engine installations do not
pay their disk or download cost. The component is disabled by default. To make it available in a
build tree and install it separately:

```sh
cmake -S . -B build -DASTRA_INSTALL_LEARNING_COMPONENT=ON
cmake --install build --component AstraLearning
```

The component installs `share/astra/learning/manifest.json` and the example sources/guides beneath
`share/astra/learning/examples`. Astra Studio discovers it beside the editor or through
`ASTRA_LEARNING_COMPONENT_DIR`. Development checkouts use `learning/manifest.json` at the repo
root. The manifest is versioned independently from the editor UI and contains the catalogue,
source paths, and optional guide paths. Example compilation remains a separate build choice via
`ASTRA_BUILD_EXAMPLES`; installing the learning component does not force example binaries into
the base profile.

## Building and packaging Astra Studio

Astra Studio uses GPUI's native application menu API for File, Edit, View, Build, and Help menus.
The editor can be built and installed alongside the C++ SDK with CMake:

```sh
cmake -S . -B build -DASTRA_BUILD_STUDIO=ON
cmake --build build --target astra_studio
cmake --install build
cpack --config build/CPackConfig.cmake
```

CPack emits a DragNDrop disk image and tar archive on macOS, ZIP and NSIS packages on Windows,
and tar, DEB, and RPM packages on Linux (the native package tools must be installed). Set
`ASTRA_BUILD_STUDIO=OFF` to package only the C++ SDK. The SDK and editor install as separate
components, and the optional learning content remains its own component.

For a native application bundle, install Cargo Bundle and run it from the Rust workspace:

```sh
cargo install cargo-bundle
cargo bundle --release -p astra-studio
```

The bundle metadata includes the macOS, Windows, and Linux icon resources and the splash image.
Cargo Bundle creates the platform application bundle; CPack packages the complete SDK install.

# Astra Studio

Astra Studio is the native Rust 2024 + GPUI desktop application for developing Astra games.
It is intentionally a project workspace rather than a web-style marketing dashboard.

## Current workflows

- Project explorer indexes actual source, shader, model, image, Blender, and project files.
- Source view opens real project files from disk (currently read-only; editing and save are next).
- Open dispatches source documents to the built-in source view, `.blend` files to Blender, and
  other assets to their macOS associated applications.
- Build invokes the repository's CMake `astra_open_world` target and captures build output.
- Play launches the built runtime in its own native Metal window.
- Selection inspector reports actual file type and on-disk size.
- Output panel shows real workspace and build/runtime messages.

## Run

From the repository root:

```sh
cargo run -p astra-studio
```

The Build action expects a configured `build/` CMake directory. Configure it from the repository
root with `cmake -S . -B build -DASTRA_BUILD_EXAMPLES=ON` if needed.

## Honest integration boundaries

The current source view is read-only; it is not yet a full text editor with editing, language
services, or save/reload. The runtime still presents in its own native window: the center pane
is a source workspace, not a fake 3D scene and not yet an embedded Metal viewport. The scene
graph, transform gizmos, material/animation authoring, and Blender round-trip importer are future
engine/editor integrations. Blender is launched as an external DCC and glTF/GLB is the current
interchange path.

The next milestones are an editable code surface with diagnostics, a serializable scene/entity
model, an embedded engine render surface, and real asset import/inspection tools.

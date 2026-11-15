# Astra Studio

Astra Studio is the native Rust 2024 + GPUI desktop application for developing Astra games.
It is intentionally a project workspace rather than a web-style marketing dashboard.

## Current workflows

- Project explorer indexes actual source, shader, model, image, Blender, and project files.
- Source view opens and edits text project files in place; use the keyboard to edit and `⌘S` to save.
- Open dispatches source documents to the built-in source view, `.blend` files to Blender, and
  other assets to their macOS associated applications.
- Build invokes the repository's CMake `astra_open_world` target and captures build output.
- Play launches the built runtime in its own native Metal window.
- Selection inspector reports actual file type and on-disk size.
- Output panel shows real workspace and build/runtime messages.
- Build runs asynchronously so the editor remains responsive while CMake works.
- Create an Astra scene JSON document, add entities with default transforms, and save the scene.
- Scene hierarchy is populated from the active Astra scene document rather than a fixed mock list.

## Run

From the repository root:

```sh
cargo run -p astra-studio
```

The Build action expects a configured `build/` CMake directory. Configure it from the repository
root with `cmake -S . -B build -DASTRA_BUILD_EXAMPLES=ON` if needed.

## Honest integration boundaries

The source editor currently provides basic keyboard editing and whole-file save, but not yet
selection/mouse caret placement, syntax-aware editing, language services, or undo history. The
runtime still presents in its own native window: the center pane
is a source workspace, not a fake 3D scene and not yet an embedded Metal viewport. The scene
graph, transform gizmos, material/animation authoring, and Blender round-trip importer are future
engine/editor integrations. Blender is launched as an external DCC and glTF/GLB is the current
interchange path.

The next milestones are a production text-editing surface with diagnostics, transform/component
editing in the inspector, an embedded engine render surface, and real asset import/inspection tools.

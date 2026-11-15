# Astra Studio

Astra Studio is the native Rust 2024 + GPUI desktop application for developing Astra games.
It is intentionally a project workspace rather than a web-style marketing dashboard.

## Current workflows

- Startup presents a short Astra Studio splash, then opens the Home page (with Learn available
  when the optional learning component is installed).
- Home provides a Continue Working action for the current workspace and a native folder picker
  for opening another project directory.
- Learn is supplied by the optional Astra Learning component. When installed, it lists the
  component's examples and guides, with source-opening and (when a configured engine build is
  available) per-example CMake build actions.
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

The Learn page is not compiled into or required by the editor. It discovers an independently
installed learning component through `ASTRA_LEARNING_COMPONENT_DIR`, the development checkout's
`learning/` directory, or the editor's adjacent `share/astra/learning` install location. Without
that component, the Learn navigation and Home shortcut are omitted.

For a development checkout, the component manifest is `learning/manifest.json`. For an installed
SDK, configure with `-DASTRA_INSTALL_LEARNING_COMPONENT=ON`, then install it separately with
`cmake --install build --component AstraLearning`. The content component contains example sources
and learning guides; it does not add dependencies to the core editor package. Example builds
additionally require an engine checkout/build configured with `-DASTRA_BUILD_EXAMPLES=ON` (and
`ASTRA_ENABLE_PHYSICS=ON` for the physics sample). Set `ASTRA_ENGINE_SOURCE_DIR` if the engine
checkout is not the current project.

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

# Examples

These executable programs are built against Astra public C++ APIs. The Core and Platform samples
are portable. The native window and renderer sample currently requires macOS AppKit and Metal.

## Available examples

| Target | Source | Demonstrates |
|---|---|---|
| astra_example_core | core_foundations.cpp | Logging, typed configuration, aligned allocation/tracking, and worker jobs |
| astra_example_platform | platform_foundations.cpp | System information, filesystem operations, and headless application lifecycle |
| astra_sandbox | sandbox/main.cpp | An integrated application combining Core and Platform services |
| astra_windowed_3d_scene | windowed_3d_scene/main.cpp | `astra::game::Game` lifecycle/input facade, indexed 3D mesh, camera transforms, depth testing, and directional lighting (macOS only) |
| astra_boar_transformation | boar_transformation/main.cpp | Matching-topology human and boar sculpts with a continuous vertex-by-vertex transformation animation (macOS only) |
| astra_oit_demo | oit/main.cpp | Intersecting translucent objects rendered with weighted blended order-independent transparency (macOS only) |
| astra_ibl_demo | ibl/main.cpp | Equirectangular HDR conversion, diffuse irradiance, GGX specular prefiltering, and split-sum BRDF lighting across metallic/roughness samples (macOS only) |
| astra_virtual_geometry_streaming | virtual_geometry_streaming/main.cpp | Hierarchical page requests, pinned coarse roots, parent-before-child residency, bounded upload queues, and LRU budget eviction |
| astra_physics_sandbox | physics/main.cpp | Jolt rigid bodies, triangle-mesh collision, constraints, raycasts, contact events, and CharacterVirtual |
| astra_open_world | open_world/main.cpp | Streamed third-person world, imported glTF character/animation, texture-backed PBR, shadows, HDR, sky/clouds/fog, physics, and virtual-geometry LOD (macOS only) |

`astra_ibl_demo` accepts an optional Radiance RGBE environment path, for example:

```sh
./astra_ibl_demo /path/to/studio.hdr
```

Without an argument it generates a small procedural HDR studio environment.

## Build and run

From the repository root:

~~~sh
cmake --preset debug
cmake --build --preset debug

./.cache/build/debug/astra_example_core
./.cache/build/debug/astra_example_platform
./.cache/build/debug/astra_sandbox
~~~

The non-interactive examples and API tests are registered with CTest when both ASTRA_BUILD_EXAMPLES and
BUILD_TESTING are enabled. Disable example targets with -DASTRA_BUILD_EXAMPLES=OFF when only
the libraries and tests are needed.

## Native window + Metal 3D scene (macOS)

~~~sh
cmake --build --preset debug --target astra_windowed_3d_scene
./.cache/build/debug/astra_windowed_3d_scene
~~~

A native resizable window opens and displays a rotating, indexed cube with per-face normals,
depth testing, and directional diffuse lighting. Close the window or press Escape to exit. The
example logs the selected Metal device.

## Boar transformation demo (macOS)

~~~sh
cmake --build --preset debug --target astra_boar_transformation
./.cache/build/debug/astra_boar_transformation
~~~

This example loads two separate, authored GLB assets: a textured 262K-triangle human and a
three-part boar model. It starts with the human fully visible, transitions into the boar using a
smooth material dissolve, holds the creature form, and reverses the transition in a loop. The
human's PBR textures and the boar's separate mesh materials are imported through Astra's glTF and
RHI paths. Press **Space** to pause/resume, **R** to restart, or **Escape** to exit. Both endpoint
GLBs can be opened directly in Blender or another glTF viewer.

## Open-world walking demo (macOS)

~~~sh
cmake --build --preset debug --target astra_open_world
./.cache/build/debug/astra_open_world
~~~

Explore an asynchronously streamed landscape with deterministic procedural grass and wind plus six
CC0 Poly Haven tree, sapling, shrub, fern, and mossy-rock models. Repeated natural assets are GPU-instanced
and frustum-culled per camera/shadow pass. The player is the 69-joint AstraPlayer with PBR materials and
Idle/Walk/Run clips. Terrain uses real Poly Haven diffuse, normal, AO, and roughness maps. Metal rendering
includes three cascaded 16-bit PCF shadow maps (1024²/768²/384²), HDR/TAA, GPU-driven terrain cluster LOD,
a procedural sky with bounded world-space volumetric cloud ray marching, fog, and Jolt CharacterVirtual collision
against streamed terrain triangle meshes when physics is enabled. Metal uses 36-byte static
vertices and 52-byte skinned vertices, with GPU-native packed tangent frames.

The demo defaults to a bounded base-M3 profile: a 1024×640-point Retina window starting at 0.65x
internal HDR/TAA rendering with full-resolution presentation. GPU-time feedback adjusts the internal
scale in coarse 0.05 steps between 0.50x and 0.85x, leaving a dead band to avoid oscillation. The
profile also uses 256px runtime vegetation textures, 512px player textures, 1K terrain textures, a
nine-chunk working set, and one retained grass LOD per chunk.
Use **WASD** or the arrow keys to move, **Shift** to sprint, **Space** to jump, the **mouse wheel** to zoom,
and hold the **left mouse button while moving the mouse** to orbit the camera. The macOS **Settings** menu
offers 50/65/80/100% internal resolution, 30/60/90/120/144/unlimited FPS caps, adaptive-resolution control,
performance telemetry, and a benchmark mode (uncapped, native internal resolution). The title bar reports
rolling FPS, CPU/GPU frame time, render scale, draw count, resident chunks, and virtual-geometry residency when
telemetry is enabled. Benchmark summaries are appended asynchronously to `astra_benchmark.csv`.
Press Escape to exit. Runtime asset variants are documented alongside their original source files in `assets/`.

This is still an experimental vertical slice, not a production renderer: full image-based lighting is available
through the reusable Metal environment API, while the open-world scene retains the analytic fallback; clouds are
a bounded procedural density field with temporal reprojection rather than a simulated 3D weather system; physics
has rigid bodies, constraints, contact events, triangle-mesh colliders and a Jolt CharacterVirtual player
controller; weighted blended OIT is order-independent but approximate; and virtual geometry uses GPU-driven
indirect-command-buffer multi-draw for opaque/OIT passes on supported Apple GPUs, with a CPU shadow/fallback path.
It still lacks GPU-feedback-driven disk asset streaming, attribute-aware production simplification, and
Nanite-style compression. See
[`docs/architecture.md`](../docs/architecture.md) for the remaining limitations and asset attribution.

# Architecture and module ownership

This document defines module boundaries and distinguishes implemented systems from planned work.
Directories are organizational homes, not proof that a full subsystem exists. A module should expose a narrow public interface
and keep implementation details private to its owner.

## Planned dependency direction

```text
Editor and developer tools
          |
        Engine
          |
  Renderer and Scene
          |
       RHI API
          |
 Platform abstraction
          |
   Platform services
```

The renderer depends on the RHI contract. A graphics backend implements that contract and
depends on platform services where required. Engine systems should not call Metal, Vulkan, or
DirectX APIs directly. Platform services expose portable interfaces; operating-system-specific
implementations live behind those interfaces. This keeps both platform services and graphics
backends replaceable without rewriting higher-level systems. Shared low-level code must not
depend on the editor or on a concrete graphics backend.

## Directory ownership

| Path | Responsibility |
| --- | --- |
| `engine/core/` | Foundational types, diagnostics, utilities, and shared runtime policies. |
| engine/platform/ | Operating-system services, portable window/event contracts, and native platform adapters. |
| engine/rhi/ | Backend-independent render-device contracts and GPU-facing types. |
| engine/renderer/ | Backend selection and high-level rendering orchestration. |
| engine/assets/ | glTF 2.0/GLB parsing, PBR material/skin/animation data, and platform image decoding. |
| engine/geometry/ | Meshlet construction, cluster hierarchy, simplified parent payloads, and screen-space LOD selection. |
| engine/world/ | Bounded asynchronous terrain chunk streaming and heightfield character movement. |
| `engine/scene/` | Scene, entity, transform, and world organization. |
| backends/metal/ | Native Metal device, pipeline, command queue, and presentation implementation. |
| `backends/vulkan/` | Future Vulkan implementation for supported desktop platforms. |
| `backends/directx12/` | Future Direct3D 12 implementation for Windows. |
| `editor/` | Future editor application and its user interface. |
| `tools/` | Developer-facing command-line tools and diagnostics. |
| `rust/` | Cargo workspace for asset tooling, package management, and suitable support tools. |
| `tests/` | Unit, integration, and system-level tests. |
| `examples/` | Small projects demonstrating supported engine workflows. |
| `third_party/` | Reviewed third-party source dependencies, only when justified. |
| `docs/` | Architecture, development, packaging, and contributor documentation. |

## Language and API boundaries

C++23 owns performance-sensitive runtime systems. Rust 2024 owns developer tools, asset
processing, package management, and supporting infrastructure where it provides a clear
benefit. Objective-C++ may be introduced at narrow Apple framework boundaries when ordinary
C++ interoperability is insufficient; it is not a project-wide requirement.

macOS, Windows, and Linux are the planned desktop platforms. Their native services belong in
`engine/platform/macos/`, `engine/platform/windows/`, and `engine/platform/linux/` respectively.
Platform-neutral interfaces stay in the parent platform module. Objective-C++ is permitted
only in Apple-specific implementation files; Windows integration may use the Windows SDK, and
Linux integration may use its native system interfaces. None of these platform types should
leak into shared engine interfaces.

Metal is the native Apple graphics backend. Vulkan is the portable backend candidate for
Windows, Linux, and potentially macOS through a portability implementation such as MoltenVK.
Direct3D 12 is a Windows-only backend candidate. Backend selection and availability belong
below the RHI contract, so a project can choose supported backends without changing gameplay,
scene, or renderer-facing APIs.

## Planned desktop platform matrix

| Platform | Native services | Graphics backend candidates |
| --- | --- | --- |
| macOS (Apple Silicon first) | Cocoa and other Apple system frameworks | Metal; Vulkan through MoltenVK where supported |
| Windows (x64 first) | Windows SDK and Win32 | Direct3D 12; Vulkan |
| Linux (x64 first) | Linux system interfaces; optional window-system integrations | Vulkan |

Only the macOS column has a working native window and rendering path today. Windows and Linux
remain planned adapters, not verified runtime targets. Window-system integrations such as
Wayland and X11 should be optional Linux platform adapters, so the engine core does not require
both. Likewise, a machine should not need every graphics SDK installed: configure and build only
the selected backend and its platform requirements.

## Current rendering and world vertical slice

The macOS Metal path now includes:

- A texture-capable RHI with RGBA8 uploads, UV0/UV1, tangents, four joint influences, and per-draw skin palettes.
- A glTF 2.0/GLB loader for triangle primitives, node hierarchies, PBR material factors, embedded/external images,
  skins, inverse-bind matrices, and STEP/LINEAR/CUBICSPLINE TRS animation channels. macOS image decoding uses
  ImageIO/CoreGraphics; the current importer accepts texture-coordinate sets 0 and 1.
- Metallic/roughness PBR with base-color, metallic/roughness, normal, occlusion, and emissive texture slots;
  mipmapped Metal textures; GGX/Fresnel lighting; configurable scene-linear directional-light radiance;
  analytic sky-environment diffuse/specular approximation.
- Three stabilized directional shadow cascades at 1024²/768²/384² with snap-margin guards, overlapping scene-caster
  depth bounds, five-tap PCF with lit out-of-map samples, and
  blended split transitions; an RGBA16Float HDR scene target; eight-sample Halton projection jitter; RGBA16Float
  motion/reactive data; finite-cloud representative-point reprojection; and motion-reprojected temporal AA with
  ping-pong HDR color/linear-distance history, previous-camera-aware depth rejection, and neighborhood clamping;
  exposure and ACES tone mapping to an sRGB drawable; world-direction sky lighting; and bounded 3D ray-marched
  cloud and height-varying fog volumes.
- glTF per-texture min/mag/mipmap/wrap sampler state; alpha-mask discard in the color and shadow passes;
  weighted blended order-independent transparency with accumulation/revealage and temporal resolve for BLEND
  materials; a sorted-alpha fallback on paths without OIT; and double-sided normal orientation.
- An executable per-frame render graph: Metal compiles pass dependencies and invokes native cascaded-shadow, sky,
  geometry/motion-vector, GTAO composition, temporal-resolve, and tone-map callbacks in graph order. Opaque shading
  writes indirect diffuse to a dedicated MRT; the depth-reconstructed horizon pass attenuates only that contribution
  before TAA. Imported backend textures
  participate in hazard tracking.
- A GPU-selected virtual-geometry hierarchy: cluster trees are packed and uploaded as resident pages; Metal compute
  selects the projected-error LOD cut using conservative transform bounds, camera-forward depth for camera passes,
  and cascade-texel error for shadow passes; culls against the jittered camera or cascade frusta; and writes indexed
  indirect arguments. On supported Apple GPUs, a second compute kernel translates those results into per-resource
  indirect command buffers, executed as a multi-draw in opaque and weighted-OIT scene passes. Shadow passes and
  unsupported devices retain the CPU-issued per-cluster indirect fallback. The open-world example uses a bounded,
  parent-first page request queue and byte-accounted residency alongside its bounded terrain chunk streamer. This is
  not Unreal Nanite: there are no mesh shaders, compressed virtual asset files, GPU-feedback-driven disk streaming,
  or production-grade topology-preserving simplifier.
- Bounded asynchronous heightfield chunk generation/eviction, per-chunk terrain meshlets uploaded as one persistent
  virtual-geometry resource, three lazily cached vegetation GPU meshes per resident chunk, LOD-scaled foliage
  density, curved multi-segment grass ribbons, and a shared world-space wind deformation evaluated consistently
  for current color, previous-frame motion and cascaded shadow positions. The open-world scene combines six scanned CC0 tree, sapling, shrub, fern, and
  mossy-rock models with deterministic grass placement; plus a capsule-footprint character controller with gravity,
  jumping, slope limits, diagonal contact samples, and sliding.

The open-world example connects these systems in one playable third-person scene. CPU/GPU resources are owned
by streamed chunk/model lifetimes; chunk count, worker concurrency, and terrain resolution have explicit bounds.
On Apple Silicon, CPU and GPU allocations share unified physical memory, so telemetry and tighter runtime
budgets remain important.

## Known limitations

- Only the macOS AppKit/Metal backend is implemented. Windows/Linux windows and graphics backends remain planned.
- HDR is used for the linear scene target and tone-mapped to an SDR sRGB drawable; direct EDR/HDR display output
  is not implemented.
- The forward renderer supports one directional key light and a bounded list of up to 32 point emitters. Point
  emitters use linear radiometric intensity, inverse-square attenuation and a smooth finite-range cutoff; they do
  not yet cast local shadows and are not clustered/tiled. The initial full-resolution GTAO path uses 8 directions × 6
  depth-reconstructed samples and affects indirect diffuse only; `SceneSettings::gtao` validates and controls its
  enablement, world-space radius, strength and bias. It has no dedicated AO history, bilateral denoiser or upsampling
  path yet. There is no dynamic global illumination, no screen-space reflections
  or local reflection probes, and no bloom or automatic exposure. Material occlusion maps are not substitutes for
  screen-space AO. Directional-light radiance is a scene setting instead of a shader constant.
- Metal exposes engine-level image-based lighting through `Renderer::create_environment`: linear RGBA32F
  equirectangular input is converted to a cubemap, mipmapped, convolved into diffuse irradiance, GGX-prefiltered
  across roughness levels, and paired with a split-sum BRDF integration LUT. PBR materials use the irradiance,
  prefiltered specular, and BRDF resources when a `GpuEnvironment` is assigned in `SceneSettings`; the analytic
  sky/ambient approximation remains the fallback. The open-world example now creates an outdoor linear HDR sky
  environment and assigns it to the scene, so terrain and vegetation use the full IBL path. Radiance RGBE decoding
  is available as a bounded memory-only asset API; file loading and environment cache serialization are left to
  applications.
- Volumetric clouds now expose reusable scene settings for altitude, density, coverage, wind velocity, enablement,
  and bounded ray-step quality. The Metal sky pass ray-marches a world-space 3D noise/height-density field and feeds
  motion and reactive coverage into temporal AA. This is not yet a temporally accumulated 3D weather simulation,
  cloud-shadow system, or weather-volume streaming pipeline.
- Temporal AA is a motion-reprojected history resolve with Halton jitter, camera-aware linear-distance depth
  rejection, and reactive weighting; it is not a temporal upscaler or dynamic-resolution system, and does not
  eliminate all transparency/disocclusion artifacts.
- Virtual geometry now has GPU-selected LOD/culling, indexed-indirect generation, and feature-gated Metal indirect
  command-buffer multi-draw for opaque and weighted-OIT scene passes. Shadow draws and unsupported hardware retain
  the CPU-issued indirect fallback. The open-world example stages terrain GPU uploads through the reusable
  thread-safe page cache, with pinned coarse roots, parent-first bounded requests, byte accounting, and LRU eviction
  policy; the terrain chunk streamer still drives actual chunk lifetime eviction, and requests are distance-driven
  rather than GPU-feedback-driven. Coarse hierarchy meshes use deterministic quadric-error edge collapses with
  sampled vertex-to-surface error estimates, but this is not an attribute-aware/manifold-constrained production
  simplifier or a full Hausdorff error bound. Mesh shaders, compressed virtual asset files, and Nanite-class
  streaming/compression remain unimplemented.
- The Jolt-backed physics module now supports fixed-step static, kinematic, and dynamic rigid bodies with sphere,
  box, capsule, and cylinder shapes, static triangle-mesh colliders, impulses, velocity control, and raycasts. It
  also exposes fixed, hinge, and distance constraints; a bounded thread-safe added/persisted contact-event queue;
  and a Jolt `CharacterVirtual` capsule controller with fixed-step movement, jump buffering, ground state, stair
  stepping, and terrain-mesh collision. The open-world player uses it against streamed triangle-mesh terrain when
  `ASTRA_ENABLE_PHYSICS` is enabled. Shape casts and a general rigid-body character motor are not yet exposed;
  disabling the physics module retains the example's legacy heightfield-controller fallback.
- glTF morph-target data is imported for POSITION, NORMAL, and TANGENT deltas with bounded
  allocation and mesh/node default weights. STEP, LINEAR, and CUBICSPLINE weight channels feed
  node poses. Metal stores immutable target deltas per mesh and current/previous weights per draw
  or instance; scene and shadow vertex paths apply morphs before skeletal skinning, and the scene
  path evaluates previous morph weights with the previous joint palette for motion vectors. The
  renderer expands mesh culling bounds by the conservative sum of each target's maximum position
  delta multiplied by the absolute current weight, including per-instance weights. The GPU regression
  kernel covers multi-target deformation, per-instance weights, skinning order, previous-frame
  positions, and command-buffer resource retention. An offscreen Metal regression also executes the
  production `astra_vertex` and `astra_shadow_vertex` entry points and checks current/previous clip
  coordinates. CPU tests cover visible-instance weight compaction, history fallback, conservative
  bounds, and glTF LINEAR/CUBICSPLINE weight-channel loading. A 143,446-vertex, two-instance, two-target,
  one-joint GPU dispatch measured 0.79 ms in the current validation run on the Apple GPU; this is the shared deformation
  helper regression kernel, not a full-scene frame-time measurement. The supplied human/boar assets
  still cannot form a valid transformation: the human is static with 143,446 vertices split across
  306 disconnected components; the domestic boar is static triangle soup with 4,542 vertices,
  1,514 triangles, no textures, and no rig. The example now displays them side by side for asset
  inspection and explicitly reports that it is not a transformation. A detailed wild-boar asset and
  authored topology/correspondence data are still required for the actual transformation scene.
  Advanced compression extensions, second skin-influence sets
  (`JOINTS_1`/`WEIGHTS_1`), and texture-coordinate sets above 1 are not implemented. Metal uses weighted blended
  order-independent transparency for glTF BLEND materials; this is approximate and does not preserve exact
  refractive transmission or physically correct colored transmittance. glTF 32-bit indices currently pass
  through a float accessor decode, so extremely large index values above float's exact-integer range need a dedicated
  integer decode path. Image decoding outside macOS is currently an explicit unsupported stub.
- The open-world scene uses scanned plant assets, but its grass is generated curved ribbon geometry and the
  deterministic species distribution remains a small demonstration biome. It does not yet have alpha-tested
  leaf-card rendering, species-specific growth/season systems, player/physics-driven foliage interaction, biome
  authoring, or GPU-driven vegetation instance culling. The outdoor IBL environment is a generated clear-sky
  approximation, not a captured forest HDR panorama; volumetric clouds are rendered separately and do not cast
  cloud shadows into the scene.

## Implemented runtime foundation

The initial C++ runtime foundation is dependency-free apart from platform SDKs:

- Core provides severity-filtered, synchronized logging; aligned allocation and live/peak memory
  tracking; a thread-safe typed configuration store; and a fixed-size thread pool with future-based
  task submission and explicit shutdown behavior.
- Platform reports system information, exposes a monotonic clock, performs basic filesystem
  operations, and provides a headless application lifecycle. On macOS it also creates AppKit
  windows, translates common keyboard/mouse/resize/close events, and exposes a CAMetalLayer
  presentation surface through an opaque handle.
- RHI defines backend-independent mesh, texture, material, skinning, scene-lighting, matrix, color, and error
  contracts. Renderer selects a supported backend without exposing graphics API types to application code.
- The Metal backend compiles MSL into a metallib, uploads indexed meshes, textures and virtual-geometry clusters,
  performs skeletal skinning, GPU cluster culling/indirect drawing, cascaded shadows, temporal AA, HDR/sky/geometry
  passes, and presents frames.
- CMake exposes Core, Platform, RHI, Renderer, Assets, Geometry, World, and the macOS-only Metal backend. CTest
  covers runtime foundations, world modules, geometry selection, glTF validation, animation sampling, and a real
  included GLB asset smoke test.

The Platform Application callback loop remains headless. Native windowing is a separate Window
API; the initial macOS adapter translates common key, mouse, resize, and close events but does not
yet provide text input, gamepads, drag-and-drop, or a unified input action system. Windows and
Linux window adapters are not implemented. Filesystem access is a basic portable layer, not a
virtual filesystem, asset database, or sandbox. Memory tracking is an allocator wrapper, not yet
a full engine memory budget/telemetry system. The thread pool is a general executor, not yet a
dependency-aware frame/job graph. The current renderer is an integrated experimental vertical
slice; its render graph executes pass callbacks but does not yet allocate/alias transient resources or emit
explicit backend barriers. It does not yet provide a production render graph, animation retargeting, asset hot
reload, or a production editor. New systems should remain behind the
existing module boundaries rather than expanding the open-world example into a monolith.

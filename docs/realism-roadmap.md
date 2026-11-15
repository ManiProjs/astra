# Astra photorealistic engine program

## Target

Build a reusable real-time 3D engine capable of shipping visually convincing, large-scale
outdoor games. The open-world nature project is the integration and visual-regression scene, not
the engine architecture itself. A feature is not complete because a shader compiles: it must be
exposed through engine APIs, integrated into the frame graph, tested on the real GPU, measured on
the target hardware, and demonstrated in the scene.

The first implementation target is macOS on Apple Silicon using Metal. That is a development
target, not a claim of cross-platform production readiness. Vulkan and Direct3D 12 require their
own complete backend implementations and image-equivalence validation.

## Honest baseline

The repository currently contains an experimental forward-rendering vertical slice:

- Metal HDR scene rendering, GGX/Schlick metallic-roughness shading, normal/AO/emissive maps,
  cascaded directional shadows, environment-map diffuse/specular IBL, TAA, weighted blended OIT,
  volumetric-looking cloud/fog approximations, and ACES-fitted tone mapping.
- glTF assets, skeletal animation, morph targets, Jolt physics, streamed heightfield chunks,
  vegetation instances, and GPU-selected virtual-geometry LOD.
- A playable third-person open-world walkthrough.

This is not yet a photorealistic engine. Important missing capabilities include real-time global
illumination, screen-space ambient occlusion, screen-space/probe reflections, multiple local
lights, robust physical atmosphere and cloud shadows, advanced material models, layered terrain,
ecological vegetation, water, automatic exposure, bloom, color grading, production asset streaming,
and a production render graph. Only the Metal backend and macOS native window path are implemented.

## Milestone 1 — physically grounded lighting and material response

- [ ] Replace shader-embedded light energy and fog constants with validated scene/environment
  parameters and a consistent linear-light convention.
- [ ] Add point and spot lights with inverse-square attenuation, physically meaningful range falloff,
  and bounded light lists; follow with clustered/tiled light culling before scaling scene counts.
- [ ] Add robust screen-space ambient occlusion (GTAO-class), temporal stabilization and bilateral
  upsampling; keep material AO as a separate input rather than treating it as screen-space AO.
- [ ] Add contact-shadow support and a scalable shadow-filtering strategy beyond fixed five-tap PCF.
- [ ] Add anisotropic texture filtering and material sampler controls; validate normal/tangent-space
  handling and texture color-space correctness.
- [ ] Add material extensions in deliberate stages: clearcoat, sheen, transmission, subsurface
  approximation for foliage/skin, anisotropy and detail normals.
- [ ] Add deterministic GPU image tests for BRDF energy, light falloff, shadow stability and AO.

## Milestone 2 — real-time indirect light and reflections

- [ ] Add local reflection probes with parallax correction and blending.
- [ ] Add screen-space reflections with confidence, roughness-aware filtering and environment fallback.
- [ ] Add a scalable real-time GI path (begin with a validated probe/voxel or screen-space solution;
  evaluate hardware ray tracing only behind device capabilities).
- [ ] Ensure direct and indirect energy are not double-counted and that AO affects indirect lighting
  appropriately rather than indiscriminately darkening direct light.
- [ ] Add reference scenes for static/dynamic objects, foliage, glossy/wet surfaces and disocclusion.

## Milestone 3 — production frame pipeline and image formation

- [ ] Make the render graph own transient texture/buffer allocation, lifetime aliasing, explicit
  resource transitions/barriers, pass validation and backend execution.
- [ ] Separate scene extraction, render scheduling, pass implementations and backend resources.
- [ ] Add histogram-based automatic exposure with temporal adaptation and exposure compensation.
- [ ] Add HDR bloom, filmic/color grading controls, robust sharpening/upscaling and optional higher
  quality anti-aliasing; retain a calibrated SDR output path and add EDR/HDR output where supported.
- [ ] Add GPU captures, per-pass timing, bandwidth/allocation telemetry and repeatable performance
  budgets on base Apple Silicon hardware.
- [ ] Validate TAA and all temporal effects under camera motion, animated objects, wind, alpha-tested
  foliage, disocclusion and dynamic resolution.

## Milestone 4 — physically convincing outdoor world

- [ ] Replace the demonstration sine/hill height function with a terrain-provider API supporting
  authored heightfields, procedural landforms, multiresolution sampling and large-world coordinates.
- [ ] Add terrain material layers driven by slope, height, biome, moisture and authored masks:
  soil, grass, rock, moss, mud, leaf litter, snow and wet surfaces.
- [ ] Add macro variation, triplanar/close-range detail, distance-aware texture resolution and
  attribute-preserving terrain LOD.
- [ ] Add a biome/ecology system with deterministic species distributions, density fields, clustering,
  growth/age variation, ground cover, fallen branches, stones and debris.
- [ ] Replace CPU-built per-chunk grass geometry with reusable GPU-driven grass patches, species
  variation, distance LOD/impostors, wind fields and player/physics interaction.
- [ ] Use one spatial wind field consistently for color, depth, shadow and previous-frame evaluation.
- [ ] Add a physically based atmosphere/sky model, weather volumes, cloud self-shadowing and cloud
  shadows on terrain/vegetation; use temporal volumetric integration with bounded quality tiers.
- [ ] Add a water system with shoreline integration, depth/color absorption, reflections, waves,
  refraction and interaction with weather/terrain.
- [ ] Move expensive chunk mesh, vegetation, collider and upload work off the frame thread; add
  predictive streaming, memory budgets, cancellation and stress tests.

## Milestone 5 — assets, tools and platform coverage

- [ ] Add an asset database with stable IDs, dependency tracking, import settings, cache keys,
  asynchronous decode/upload, texture mip streaming and hot reload.
- [ ] Add quality-preserving mesh LOD/HLOD, GPU-driven instance culling and production virtual-asset
  storage/streaming.
- [ ] Extend import support deliberately for required glTF material/geometry extensions, compression,
  additional skin influence sets and higher texture-coordinate sets.
- [ ] Add a licensed, high-quality validation environment with scanned terrain, vegetation, water,
  architecture and detailed animated characters.
- [ ] Implement Vulkan and Direct3D 12 backends against the same RHI contracts and validate image
  output and synchronization behavior on each backend.
- [ ] Add a practical editor, material/terrain/biome authoring, profiling, resource inspection and
  diagnostics workflow.

## Immediate implementation order

1. Establish correct scene-light controls and fix deformation consistency across color, history and
   shadow passes.
2. Add GTAO using a depth prepass and a properly integrated AO pass; test it on real Metal frames.
3. Add point/spot lights and a bounded light-list contract, then clustered culling.
4. Add automatic exposure and bloom after TAA.
5. Build terrain layers and biome-driven world generation; move those systems out of the example.
6. Add reflection/GI and physical atmosphere/cloud shadows, then water.

## Completion gate

For every milestone: build the affected targets with Clang, run focused unit/integration tests,
execute the actual GPU path, capture and inspect before/after frames, measure GPU/CPU time and memory
on the target Apple Silicon machine, test camera movement and temporal stability, and document limits.
Do not call a generated gradient an HDRI capture, material AO screen-space AO, environment reflections
dynamic reflections, or a playable walkthrough a finished open-world game. Do not claim
photorealism without repeatable visual evidence against a high-quality reference scene.

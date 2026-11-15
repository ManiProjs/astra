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
- [x] Add a bounded point-light list (32 emitters) with linear radiant-intensity units,
  inverse-square attenuation and smooth finite-range cutoff. Spot lights, local shadows and
  clustered/tiled culling remain open before scaling scene counts.
- [x] Add an initial full-resolution, depth-reconstructed horizon AO pass (8 screen directions × 6
  radial samples) and apply it only to the separately rendered indirect-diffuse contribution. The
  existing TAA resolves the composited result; dedicated AO history, depth/normal-aware denoising,
  bilateral upsampling and GPU image regression remain open before production use.
- [ ] Add contact-shadow support and a scalable shadow-filtering strategy beyond fixed five-tap PCF.
- [ ] Add anisotropic texture filtering and material sampler controls; validate normal/tangent-space
  handling and texture color-space correctness.
- [ ] Add material extensions in deliberate stages: clearcoat, sheen, transmission, subsurface
  approximation for foliage/skin, anisotropy and detail normals.
- [ ] Add deterministic GPU image tests for BRDF energy, light falloff, shadow stability and AO.

## Milestone 2 — real-time indirect light and reflections

- [ ] Add local reflection probes with parallax correction and blending.
- [ ] Add screen-space reflections with confidence, roughness-aware filtering and environment fallback.
- [ ] Add a bounded one-bounce screen-space diffuse GI (SSGI) path using explicit direct-radiance and
  receiver-albedo buffers, depth-aware ray marching, confidence fades, temporal reprojection and
  the existing global IBL as an off-screen fallback. Validate half-resolution quality and cost before
  enabling it by default; screen-space misses/leaks remain inherent limitations.
- [ ] Evolve beyond screen space with a scalable probe/voxel GI solution; evaluate hardware ray
  tracing only behind device capabilities and explicit acceleration-structure support.
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
2. Validate and improve the integrated depth-driven GTAO pass on real Metal frames; add dedicated
   temporal AO history, edge-aware filtering and image regression.
3. Add spot lights, local shadows and clustered culling for the bounded light-list contract.
4. Prototype one-bounce SSGI on top of the new indirect-diffuse resource, with IBL fallback.
5. Add automatic exposure and bloom after TAA.
6. Build terrain layers and biome-driven world generation; move those systems out of the example.
7. Add reflection probes/SSR and a scalable GI solution, then physical atmosphere/cloud shadows and water.

## Completion gate

For every milestone: build the affected targets with Clang, run focused unit/integration tests,
execute the actual GPU path, capture and inspect before/after frames, measure GPU/CPU time and memory
on the target Apple Silicon machine, test camera movement and temporal stability, and document limits.
Do not call a generated gradient an HDRI capture, material AO screen-space AO, environment reflections
dynamic reflections, or a playable walkthrough a finished open-world game. Do not claim
photorealism without repeatable visual evidence against a high-quality reference scene.

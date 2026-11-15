# Metal backend

The native graphics backend is implemented for macOS. CMake compiles
`shaders/scene.metal` into a Metal AIR object and `.metallib`; the backend loads
that library and creates pipelines for scene geometry, directional depth
shadows, procedural sky, horizon-based GTAO composition, temporal resolve, and
HDR tone mapping, plus compute pipelines for virtual-geometry selection/culling
and environment preprocessing.

The scene path supports indexed meshes, RGBA8 textures with sRGB/linear formats
and generated mipmaps, UV0/UV1, glTF per-texture sampler state, tangent-space
normal maps, metallic/roughness, occlusion and emissive maps, alpha masking,
weighted blended order-independent transparency, double-sided normal handling,
GPU skinning/morphing, GGX/Schlick direct lighting with configurable directional
radiance and up to 32 validated world-space point emitters (radiant intensity,
inverse-square attenuation and smooth finite-range cutoff), three 16-bit directional shadow cascades (1024²/768²/384²) with
PCF and blended transitions, an RGBA16F scene target, bounded 3D ray-marched
cloud and fog approximations, and ACES-fitted tone mapping to an sRGB drawable.
The backend also supports equirectangular HDR environment conversion, diffuse
irradiance convolution, GGX specular prefiltering and a split-sum BRDF LUT; an
environment can be assigned through `SceneSettings`. Temporal AA uses an
eight-sample Halton projection jitter, scene/sky
RGBA16F motion/reactive data, finite-cloud representative-point reprojection,
ping-pong HDR color/linear-distance history, previous-camera-aware depth
rejection, and a 3x3 neighborhood clamp before tone mapping. Transparent
fragments mark their pixels reactive instead of blending motion vectors against
opaque depth; animated clouds also reduce history contribution.

The opaque fragment path writes its indirect-diffuse IBL contribution to a
separate RGBA16F attachment. A full-resolution screen-space horizon pass
reconstructs world positions from scene depth, searches eight directions with
six radial samples each, and attenuates that indirect-diffuse term before TAA.
Direct directional/point lighting, emissive output and indirect specular remain
untouched by screen-space AO. The normal is estimated from neighboring depth
reconstructions. TAA accumulates the final composited image, but there is not yet
a dedicated AO history, depth-aware denoiser, bilateral upsampler or AO-specific
disocclusion validation; this is an initial GTAO implementation, not a finished
production AO solution or global illumination. `SceneSettings::gtao` controls
enablement, world-space radius, strength and self-occlusion bias; the RHI rejects
non-finite or out-of-range values.

Terrain virtual geometry uploads persistent vertex/index/cluster buffers per
resident chunk. A Metal compute kernel chooses the screen-space-error LOD cut,
uses transform-conservative bounds/error and camera-forward depth for camera
passes, and uses cascade-texel error for shadow passes. It frustum-culls clusters
against the jittered camera or active shadow cascade and writes
`MTLDrawIndexedPrimitivesIndirectArguments`; the render encoder submits those
indirect draws. The current implementation still encodes one indirect draw per
cluster on the CPU and is not a Metal indirect-command-buffer or mesh-shader
multi-draw pipeline.

Shadow cascade fitting reserves a diagonal half-texel snap guard and extends
each cascade's light-space depth interval to include scene caster bounds whose
light-space footprints overlap the receiver slice. Orthographic sky cameras
use parallel rays from their near plane and camera-relative sky reprojection.
PCF taps that
fall outside the shadow map contribute fully lit visibility rather than
repeating clamped edge texels.

The per-frame Metal pass sequence is compiled and executed through
`astra::rhi::RenderGraph`; its callbacks encode the native Metal commands.
Imported backend-owned resources participate in hazard ordering and lifetime
tracking.

This is not yet a production renderer. It has no spot lights,
local-light shadows, dynamic GI, screen-space reflections, reflection probes, automatic
exposure, bloom, color grading, or HDR/EDR display output. TAA is not a temporal
upscaler. Cloud and fog volumes use a small fixed
ray-march sample count rather than a temporally accumulated 3D density
simulation. Transparent sorting is object-center back-to-front and does not
solve intersecting transparency or order-independent transparency. The render
graph does not yet allocate/alias transient resources or emit explicit backend
barriers. Virtual geometry does not yet have GPU-feedback-driven disk streaming,
mesh shaders, or a production Nanite-style cluster compression/topology pipeline.

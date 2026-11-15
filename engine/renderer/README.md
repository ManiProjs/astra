# Renderer

The renderer selects an available RHI backend and exposes backend-neutral mesh,
texture, material, and scene-submission APIs. The current macOS implementation
submits multiple draw items with per-object transforms, texture-backed PBR
materials, optional skin palettes, camera/light data, and scene timing.

The Metal backend builds and executes a per-frame `astra::rhi::RenderGraph`.
Pass callbacks encode three directional shadow cascades, procedural sky, HDR
geometry with motion/reactive data, temporal history resolve, and tone mapping in
dependency order. TAA uses previous-camera-aware linear-distance rejection and
reduces history for transparent/cloud-reactive pixels. Resource declarations
track hazards and lifetimes for imported backend-owned textures. The graph does
not yet allocate transient GPU resources,
alias compatible lifetimes, or emit backend barriers.
Terrain virtual geometry uploads its complete cluster hierarchy once per chunk;
Metal compute performs transform-conservative projected-error LOD selection and
frustum culling; camera passes use the jittered view and shadow passes use
cascade-texel error. The renderer submits indexed indirect draws for selected
clusters.
The RHI still contains an older `RenderDevice` path alongside a newer low-level
`Device` contract, and Metal implements only the former.

The GLFW/OpenGL backend is an initial clear-and-present path for Windows and Linux. It creates a
real OpenGL context and presents the default framebuffer; mesh, texture, environment, and non-empty
scene rendering are explicitly unsupported until their RHI implementations are ported.

The Vulkan backend is a separate clear-and-present implementation of the live `RenderDevice`
contract. It creates the instance/device/queue/surface/swapchain/render pass/command buffers and
synchronization objects, then clears and presents. GLFW Win/Linux windows must be created with
`WindowClientAPI::vulkan`; macOS uses the AppKit CAMetalLayer through `VK_EXT_metal_surface` when
MoltenVK exposes that extension. Mesh, texture, virtual-geometry, environment, and scene drawing
are explicitly unsupported. CMake builds this backend only when `find_package(Vulkan)` succeeds
and the platform surface adapter is available.

The current renderer is an experimental vertical slice. It does not yet provide
prefiltered HDR image-based lighting, GPU-generated multi-draw command buffers,
GPU-driven world streaming, a production resource cache, a general material
graph, or a complete cross-platform backend matrix. Temporal AA is a motion-
reprojected history resolve, not a temporal upscaler or dynamic-resolution system.
GPU resources must be created by and submitted to their originating renderer;
keep the renderer and its window alive while rendering.

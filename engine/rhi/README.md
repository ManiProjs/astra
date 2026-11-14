# Rendering Hardware Interface (RHI)

RHI owns backend-independent GPU contracts. The public API is intentionally split into focused
headers under `include/astra/rhi`:

- `types.hpp` — backend-independent scalar, vector, matrix, extent, and error types.
- `descriptors.hpp` — typed texture, buffer, and sampler descriptions and usage flags.
- `resource.hpp` — abstract GPU resources and upload payloads.
- `binding.hpp` — explicit resource binding layouts and bind groups.
- `pipeline.hpp` — shader modules, vertex layouts, raster/depth/blend state, and pipeline
  descriptions.
- `command.hpp` — render-pass attachments and command-buffer/encoder contracts.
- `render_graph.hpp` — declarative resource/pass dependencies, hazard validation, topological
  scheduling, and resource lifetime analysis.
- `device.hpp` — device capabilities, resource/pipeline creation, command-buffer creation, and
  submission.
- `rhi.hpp` — optional umbrella include; clients can include only the individual contracts they use.

`render_device.hpp` remains the compatibility API used by the existing renderer and Metal backend.
The new `Device` interface is a contract layer; the current Metal backend has not yet been migrated
to it. The render graph currently compiles dependencies and lifetimes but does not allocate GPU
resources, insert backend barriers, alias transient memory, or execute pass callbacks. Those are
subsequent backend/runtime stages. RHI headers must not expose Metal, Vulkan, or Direct3D types.

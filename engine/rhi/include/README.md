# RHI public headers

Public backend-independent rendering contracts live here. The modern API is divided into
`types.hpp`, `descriptors.hpp`, `resource.hpp`, `binding.hpp`, `pipeline.hpp`, `command.hpp`,
`render_graph.hpp`, and `device.hpp`; `rhi.hpp` is an optional umbrella include. The older
`render_device.hpp` remains available as a compatibility surface during backend migration.
Backend-native types must remain outside these headers.

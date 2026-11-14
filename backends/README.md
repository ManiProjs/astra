# Graphics backends

Concrete graphics API implementations live here and implement the engine RHI. The Metal backend
is implemented for macOS and is the first supported rendering path. Vulkan and Direct3D 12 remain
planned backends. Backend code must remain behind the RHI boundary so additional APIs can be
introduced independently.

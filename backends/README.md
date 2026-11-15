# Graphics backends

Concrete graphics API implementations live here and implement the engine RHI. Metal provides the
full current macOS renderer. The optional GLFW/OpenGL path on Windows and Linux creates an OpenGL
3.2 context, clears the default framebuffer, and presents it. Vulkan provides a separate
clear/present path on GLFW Win/Linux and AppKit/MoltenVK macOS surfaces. Both initial portable
backends explicitly reject GPU resource creation and scene drawing; see each backend README for
its exact implemented scope. Direct3D remains unimplemented.

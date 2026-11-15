# OpenGL backend

The optional OpenGL backend is a first presentation slice for GLFW-backed Windows and Linux
windows. It creates and validates the current OpenGL context, clears the default framebuffer, and
swaps the GLFW buffers. The renderer reports the actual OpenGL renderer and version strings.

GPU mesh and texture creation, virtual geometry, image-based lighting, and non-empty scene drawing
return `unsupported_backend`. This backend is not feature-equivalent to Metal. CMake requires GLFW
3.3+ and the platform OpenGL development library for Windows and Linux builds. On macOS it may be
built when those dependencies are installed, but the native AppKit window currently exposes a
CAMetalLayer and does not select this OpenGL device.

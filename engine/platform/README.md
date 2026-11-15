# Platform

Platform owns portable operating-system services and the native adapters behind them.

Implemented:
- Application lifecycle callbacks (headless).
- Filesystem and basic system information.
- A portable Window contract with an AppKit/CAMetalLayer implementation on macOS and a GLFW
  window adapter on Windows and Linux.
- GLFW windows select their client API at creation: OpenGL remains the default, while Vulkan
  callers request `WindowClientAPI::vulkan` to create a `GLFW_NO_API` window suitable for
  `glfwCreateWindowSurface`. AppKit always exposes its CAMetalLayer for Metal or Vulkan.
- Common key, mouse, scroll, resize, and close event translation on all implemented adapters.
- Native Settings menu items and window-title updates on macOS; menu selections are delivered
  through the same event/input path as keyboard and mouse input.
- Presentation display-sync control on the native CAMetalLayer and GLFW OpenGL context.

The open-world example disables display sync and paces frames with its FPS cap (60 by default;
the Unlimited menu item selects zero). Benchmark mode runs at full render scale with unlimited
pacing and restores the previous scale, cap, adaptive-resolution, telemetry, and sync settings
when it ends. Each completed benchmark appends one row to `astra_benchmark.csv` in the process
working directory. Its stable columns are `elapsed_seconds`, `frames`, `average_fps`, `avg_cpu_ms`,
`avg_gpu_ms`, `render_scale`, `draw_count`, and `resident_chunks`. CSV writing runs on a background
worker and does not perform file I/O in the frame loop.

Windows and Linux require GLFW 3.3+ and use its native Win32 or selected Linux window-system
integration. GLFW must be initialized, polled, and destroyed on the process main thread.
Native menus are currently implemented only by the macOS adapter.

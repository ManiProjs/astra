# Platform

Platform owns portable operating-system services and the native adapters behind them.

Implemented:
- Application lifecycle callbacks (headless).
- Filesystem and basic system information.
- A portable Window contract with an AppKit/CAMetalLayer implementation on macOS.
- Common key, mouse, scroll, resize, and close event translation on macOS.
- Native Settings menu items and window-title updates on macOS; menu selections are delivered
  through the same event/input path as keyboard and mouse input.
- Presentation display-sync control on the native CAMetalLayer; unsupported adapters accept the
  setting as a no-op.

The open-world example disables display sync and paces frames with its FPS cap (60 by default;
the Unlimited menu item selects zero). Benchmark mode runs at full render scale with unlimited
pacing and restores the previous scale, cap, adaptive-resolution, telemetry, and sync settings
when it ends. Each completed benchmark appends one row to `astra_benchmark.csv` in the process
working directory. Its stable columns are `elapsed_seconds`, `frames`, `average_fps`, `avg_cpu_ms`,
`avg_gpu_ms`, `render_scale`, `draw_count`, and `resident_chunks`. CSV writing runs on a background
worker and does not perform file I/O in the frame loop.

Windows and Linux currently compile an explicit unsupported-window adapter. Their Win32,
Wayland, and X11 window integrations are not implemented. Window creation, event polling, and
destruction must occur on the platform's UI thread (the main thread on macOS).

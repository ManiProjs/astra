# macOS platform implementation

The macOS adapter uses AppKit for native windows and events, and CAMetalLayer as the opaque
presentation surface consumed by the Metal backend. It supports high-DPI drawable sizing,
resizing, close notifications, keyboard events, mouse buttons/motion, and scroll deltas.
Applications can install a native menu and update the window title for lightweight settings and
runtime telemetry. Menu selections are exposed as platform events, not AppKit objects.

Window creation, event polling, and destruction are main-thread-only. Text input/IME, gamepads,
drag-and-drop, multiple-window orchestration, and a unified input action system are future work.

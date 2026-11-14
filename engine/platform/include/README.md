# Platform public interfaces

Portable interfaces to operating-system services such as windows, input, files, threads,
synchronization, clocks, and dynamic libraries live here. These interfaces must not expose Win32,
Cocoa, X11, or Wayland types. Current APIs cover system information, filesystem access, and the
headless application lifecycle.

# Linux platform implementation

The portable Window API currently builds an explicit unsupported-platform implementation here.
Native Wayland and X11 window creation, event dispatch, input translation, and Vulkan-surface
integration are not implemented yet. Future integrations should make Wayland/X11 optional.

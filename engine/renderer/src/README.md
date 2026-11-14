# Renderer implementation

renderer.cpp performs backend selection and owns the RHI render device. The first available
implementation is Metal on macOS. Renderer code does not include native graphics API headers.

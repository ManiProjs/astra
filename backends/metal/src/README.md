# Metal backend implementation

metal_device.mm contains the Objective-C++ Metal device, shader compilation, render-pipeline,
command-buffer, drawable acquisition, and presentation implementation. It consumes the opaque
CAMetalLayer supplied by the macOS Platform adapter.

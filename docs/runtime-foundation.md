# Runtime foundation API

The initial runtime services are available from the `Astra::Core` and `Astra::Platform`
libraries. They use C++23 and the standard library; no third-party runtime dependency is needed.

## Logging

```cpp
#include <astra/core/log.hpp>

astra::core::Logger logger{astra::core::LogLevel::debug};
logger.log(astra::core::LogLevel::info, "Renderer", "Runtime initialized");
logger.set_level(astra::core::LogLevel::warning);
```

Records are synchronized and written to `std::clog`. The logger currently has one process-wide
output stream; configurable sinks, structured fields, timestamps, and asynchronous logging are
future work.

## Allocation and tracking

```cpp
#include <astra/core/allocator.hpp>

astra::core::DefaultAllocator system_allocator;
astra::core::TrackingAllocator tracked_allocator{system_allocator};
{
    astra::core::UniqueAllocation block{tracked_allocator, 4096, 64};
    // Use block.get() for the lifetime of this scope.
}
const auto stats = tracked_allocator.stats();
```

Allocators are non-owning services: an allocator must outlive every allocation made through it.
`UniqueAllocation` owns raw storage, not C++ object lifetimes; placement construction and object
destruction remain the caller's responsibility. The tracker reports current bytes, peak bytes,
live allocations, and total successful allocations. It is not yet a frame/GPU budget manager.

## Configuration

```cpp
#include <astra/core/config.hpp>

astra::core::ConfigStore config;
config.set_default("renderer.vsync", true);
config.set("renderer.width", std::int64_t{1920});
const bool vsync = config.get_or<bool>("renderer.vsync", false);
```

Values are exactly typed as `bool`, `std::int64_t`, `double`, or `std::string`. A type mismatch
returns an empty `std::optional`; no implicit conversions or persistence format are provided.

## Jobs

```cpp
#include <astra/core/thread_pool.hpp>

astra::core::ThreadPool jobs; // hardware_concurrency(), with a minimum of one worker
auto result = jobs.submit([](int a, int b) { return a + b; }, 20, 22);
const int value = result.get();
jobs.shutdown(astra::core::ShutdownMode::drain);
```

The default shutdown drains accepted work and joins workers. `cancel_pending` discards queued
tasks but lets already-running tasks finish; their futures report a broken promise. Task
exceptions are delivered through their futures. Do not destroy or shut down a pool from one of
its own worker threads.

## Filesystem

```cpp
#include <astra/platform/filesystem.hpp>

namespace fs = astra::platform::filesystem;
auto created = fs::create_directories("Saved/Logs");
if (created) {
    auto written = fs::write_text_file("Saved/Logs/runtime.txt", "Astra started\n");
}
```

Filesystem functions return `std::expected` with `std::error_code` for ordinary I/O failures.
They do not create parent directories implicitly, normalize project asset paths, enforce a
sandbox, or provide atomic/transactional writes. `read_file` returns bytes; `read_text_file`
returns the file's bytes as a string without text-encoding conversion.

## Application lifecycle

```cpp
#include <astra/platform/application.hpp>

astra::platform::Application app(
    {.name = "Astra Sandbox"},
    {
        .on_initialize = [] { /* initialize runtime services */ },
        .on_update = [](const astra::platform::FrameContext& frame) {
            // Update using frame.delta_time and frame.frame_index.
            return frame.frame_index < 3; // false requests normal exit
        },
        .on_shutdown = [] { /* release runtime services */ },
    });

if (app.initialize()) {
    (void)app.run();
    app.shutdown();
}
```

Lifecycle methods run on one application thread. `request_shutdown()` is safe to call from
another thread and asks the run loop to exit after its current update callback. Callback
exceptions are captured in `failure()` and move the application to `failed`. This is a headless
loop: native windows, event pumping, input, frame pacing, and platform-specific startup are not
part of this API yet.

## Runnable examples

The repository contains five buildable programs under examples/ on macOS; Windows and Linux build
the three portable programs:

- astra_example_core demonstrates configuration, logging, allocation tracking, and jobs.
- astra_example_platform demonstrates system information, filesystem access, and application
  lifecycle callbacks.
- astra_sandbox combines the Core and Platform APIs into a short headless application.
- astra_windowed_3d_scene opens a native window and presents an indexed, lit, depth-tested Metal cube on macOS.
- astra_open_world generates a procedural outdoor landscape with a third-person animated player model and a Metal PBR-style shader on macOS.

Build the examples with cmake --build --preset debug, then run the non-interactive executables
from .cache/build/debug/. The 3D scene and open-world walkthrough are interactive and must be launched manually.

## Native windowing and first renderer

The separate astra::platform::Window API creates a native window and exposes queued close,
resize, keyboard, mouse-button, mouse-motion, and scroll events. The macOS implementation uses
AppKit and a CAMetalLayer; the opaque native surface is intended only for backend integration.
Window creation, event polling, and destruction must happen on the main thread.

The astra::rhi module defines backend-independent backend identifiers, mesh vertices and indices,
matrices, colors, render errors, and a RenderDevice contract. astra::renderer::Renderer chooses an
available backend, uploads persistent mesh buffers, and submits multiple transformed meshes in
one scene pass without exposing Metal types to application code. The Metal backend compiles MSL
shaders, creates a depth-tested pipeline and depth attachment, and supports per-vertex color with
directional diffuse lighting.

Run the interactive example on macOS:

~~~sh
cmake --build --preset debug --target astra_windowed_3d_scene
./.cache/build/debug/astra_windowed_3d_scene
~~~

This is a first 3D rendering slice, not a complete game renderer. General-purpose textures and
samplers, material authoring, physically based shading, shadows, skeletal animation, post-processing,
render graphs, asset importing/streaming, Vulkan, Direct3D 12, and native Windows/Linux window
adapters are not implemented yet.

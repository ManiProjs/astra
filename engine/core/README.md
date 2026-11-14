# Core

Ownership: foundational runtime types, diagnostics, utilities, and shared policies. Core must
remain independent of the editor, operating-system APIs, and concrete graphics backends.

Core currently provides:

- A thread-safe, threshold-based logger (`astra/core/log.hpp`).
- Aligned allocation through `Allocator`, `DefaultAllocator`, and move-only `UniqueAllocation`,
  plus `TrackingAllocator` for live/peak/total allocation statistics.
- A thread-safe typed `ConfigStore` for boolean, integer, floating-point, and string settings.
- A fixed-size `ThreadPool` with futures, exception propagation, draining, and explicit
  cancellation of queued tasks during shutdown.

These facilities use only the C++ standard library. Platform-specific output sinks and editor
integrations remain outside Core.

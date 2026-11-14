# Shared platform implementation

Platform-independent service logic shared by the macOS, Windows, and Linux implementations lives
here. Native API calls belong in the matching platform subdirectory. Current implementations use
the C++ standard library for system information, monotonic time, filesystem operations, and the
headless application lifecycle.

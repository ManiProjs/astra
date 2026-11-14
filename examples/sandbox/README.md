# Astra sandbox

`main.cpp` is the integrated runtime example. It creates a headless `Application`, configures
logging and runtime settings, allocates aligned scratch memory, submits background jobs, writes
a session file, and runs a short update loop.

Build and run it from the repository root:

```sh
cmake --preset debug
cmake --build --preset debug --target astra_sandbox
./.cache/build/debug/astra_sandbox
```

The sandbox is intentionally finite and headless. It exits after the configured number of update
callbacks; it does not open a window or initialize a graphics API.

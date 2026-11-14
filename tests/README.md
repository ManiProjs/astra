# Tests

Cross-module runtime foundation tests live here. Unit tests should normally live with their
owning C++ or Rust module. CTest validates Core allocation/tracking, configuration, logging,
thread-pool behavior, and Platform filesystem/application lifecycle APIs without external test
dependencies.

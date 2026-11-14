# Installation and packaging policy

The engine is intended to be distributed as a modular installation. These figures are
installed-size budgets, not compressed download sizes.

| Profile | Target installed size |
| --- | ---: |
| Minimal | 3–5 GB |
| Standard | 10–15 GB |
| Complete | 15–20 GB |

**Hard cap:** a Standard or Complete installation must not exceed 20 GB under the accounting
rules below. The Standard profile is the normal recommended installation. Optional modules
and content can be installed separately.

## Accounting rules

- Measure the logical installed size of all files owned by the selected Astra profile after
  extraction and installation. Do not use compressed archive size.
- Include the engine runtime, editor, bundled developer tools, required runtime libraries,
  profile-selected templates, and all mandatory profile packages.
- Include package data, shaders, metadata, and support files installed by Astra.
- Exclude the operating system, Xcode, Command Line Tools, Apple SDKs (including Metal), GPU
  drivers, and other platform components supplied and maintained by the operating system.
- Exclude user projects, imported game assets, build intermediates, shader caches, logs, and
  user-generated content. Report these separately when measuring disk use.
- Count shared files once when profiles share the same installed file. Count separately
  installed copies separately.
- Keep optional downloads outside a profile's required-size total until selected for install.
- Publish a machine-readable package manifest with each release so profile totals can be
  reproduced. The installer must reject a Standard or Complete plan whose computed installed
  total exceeds 20 GB.

## Future package manager requirements

The package manager is planned to support optional components, dependency resolution, package
integrity verification, signed metadata, stable/beta/development channels, incremental and
transactional updates, rollback, and cache management. Package metadata should declare
installed-size contributions and dependencies so the installer can calculate profile totals
before changing the installation.

No package manager, package format, installer, or downloadable content is implemented in this
repository yet.

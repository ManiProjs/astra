# Vulkan backend (planned)

Reserved for a Vulkan implementation of the RHI on supported desktop platforms. Vulkan is the
portable graphics backend candidate for Windows and Linux, with macOS support possible through
a portability implementation such as MoltenVK. SDK discovery and platform availability checks
will be local to this backend so other backends do not require the Vulkan SDK. No backend code
is implemented yet.

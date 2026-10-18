# macOS arm64 slice (docs/PLATFORM_MACOS.md "Build"): one architecture per build tree, so Jolt picks its
# SIMD flags from CMAKE_SYSTEM_PROCESSOR (NEON).
# The build runs tools it builds (vgeo content): on Apple Silicon the x86_64 ones run under Rosetta.
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "macOS architecture of this build tree")

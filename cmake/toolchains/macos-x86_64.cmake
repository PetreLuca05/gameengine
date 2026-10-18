# macOS x86_64 slice (docs/PLATFORM_MACOS.md "Build"): one architecture per build tree, so Jolt picks its
# SIMD flags from CMAKE_SYSTEM_PROCESSOR (SSE4.2 / AVX / F16C, no AVX2 / FMA for macOS 11 hardware).
# The build runs tools it builds (vgeo content): on Apple Silicon the x86_64 ones run under Rosetta.
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_OSX_ARCHITECTURES x86_64 CACHE STRING "macOS architecture of this build tree")

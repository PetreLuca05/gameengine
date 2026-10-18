#!/usr/bin/env bash
# ctest regex of the tests that fail on main for reasons outside the platform work, per CI platform
# (.github/workflows/platforms.yml runs them in a non-gating step so they stay visible). Remove a name
# here when its test is fixed.
#   ground_textures  generated grass / fern colours outside the target tolerances (content, all platforms)
#   phone            "page: assets injected" (tests/test_phone.c, the web/ page check)
#   vgeo_gpu         occlusion / two-pass counts on software rasterisers (llvmpipe; no-GPU runners skip)
# usage: known_failures.sh linux|windows|macos
case "${1:-linux}" in
    *) echo "^(ground_textures|phone|vgeo_gpu)$" ;;
esac

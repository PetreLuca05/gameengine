#!/usr/bin/env bash
# ctest regex of the tests that fail on main for reasons outside the platform work, per CI platform
# (.github/workflows/platforms.yml runs them in a non-gating step so they stay visible). Remove a name
# here when its test is fixed.
#   phone            "page: assets injected" (tests/test_phone.c, the web/ page check)
# usage: known_failures.sh linux|windows|macos
case "${1:-linux}" in
    *) echo "^(phone)$" ;;
esac

#!/usr/bin/env bash
# Universal (x86_64 + arm64) build (docs/PLATFORM_MACOS.md "Build"): one build tree per architecture
# (build-mac-arm64, build-mac-x86_64; also the single-arch dev trees), then lipo into
# build-mac-universal/bin with the arm64 tree's data (shaders, assets, data, web: identical in both).
# Run on Apple Silicon: the x86_64 tree runs its build-time tools (vgeo content) under Rosetta; an
# Intel Mac cannot run the arm64 tree's tools.
#
# usage: tools/macos/build_universal.sh [Release|RelWithDebInfo|Debug] [extra cmake args...]
#   ARCHS="arm64"  only one slice (the dev fast path: same trees, no lipo)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CONFIG="${1:-Release}"
[ $# -gt 0 ] && shift
ARCHS="${ARCHS:-arm64 x86_64}"
GEN=()
command -v ninja > /dev/null && GEN=(-G Ninja)

for arch in $ARCHS; do
    tree="$ROOT/build-mac-$arch"
    echo "== $arch -> $tree ($CONFIG)"
    cmake -S "$ROOT" -B "$tree" "${GEN[@]}" -DCMAKE_BUILD_TYPE="$CONFIG" \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchains/macos-$arch.cmake" "$@"
    cmake --build "$tree" --config "$CONFIG"
done

set -- $ARCHS
if [ $# -lt 2 ]; then
    echo "single architecture: $ROOT/build-mac-$1/bin"
    exit 0
fi
out="$ROOT/build-mac-universal/bin"
rm -rf "$out"
mkdir -p "$out"
first="$ROOT/build-mac-$1/bin"
# every Mach-O executable of the first tree that exists in all trees
for f in "$first"/*; do
    [ -f "$f" ] && [ -x "$f" ] || continue
    file "$f" | grep -q "Mach-O" || continue
    name="$(basename "$f")"
    slices=()
    for arch in $ARCHS; do
        [ -f "$ROOT/build-mac-$arch/bin/$name" ] && slices+=("$ROOT/build-mac-$arch/bin/$name")
    done
    [ ${#slices[@]} -eq $# ] || continue
    lipo -create -output "$out/$name" "${slices[@]}"
done
for d in shaders assets data web; do
    [ -d "$first/$d" ] && cp -R "$first/$d" "$out/"
done
for f in libsteam_api.dylib steam_appid.txt; do
    [ -f "$first/$f" ] && cp "$first/$f" "$out/"
done
lipo -info "$out/deadground"
echo "universal: $out"

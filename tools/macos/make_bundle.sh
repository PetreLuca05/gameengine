#!/usr/bin/env bash
# Deadground.app from a build's bin directory (docs/PLATFORM_MACOS.md "App bundle"):
#   Contents/MacOS/deadground            the (universal) executable, rpath @executable_path/../Frameworks
#   Contents/Resources/{shaders,assets,data,web}   SDL_GetBasePath() inside a bundle
#   Contents/Resources/Deadground.icns   from tools/macos/AppIcon.png (sips + iconutil)
#   Contents/Frameworks/libMoltenVK.dylib   SDL loads it first (no Vulkan loader needed to run)
#   Contents/Frameworks/libsteam_api.dylib  when the build has Steam (universal in the Steamworks SDK)
#   Contents/Info.plist                   tools/macos/Info.plist.in
#
# usage: tools/macos/make_bundle.sh <bin dir> <output dir> [--moltenvk <libMoltenVK.dylib>]
#        [--bundle-id com.example.deadground] [--version 0.1.0] [--build N] [--steam-appid 480] [--with-test-content]
# The generated vgeo test scenes of the dev map vgeo_test (assets/vgeo/{rock_field,statue,building}.vgeo,
# about 680 MB, rebuilt by every build) are left out unless --with-test-content.
# MoltenVK: --moltenvk, else $VULKAN_SDK/macOS/lib/libMoltenVK.dylib, else Homebrew's. A universal app
# needs a universal MoltenVK (the Vulkan SDK's is; Homebrew's is the host architecture only).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$1"; OUT="$2"; shift 2
MVK=""; BUNDLE_ID="com.deadground.game"; BUILD="1"; STEAM_APPID=""; TEST_CONTENT=0
VERSION="$(sed -n 's/^project(Deadground VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
while [ $# -gt 0 ]; do
    case "$1" in
        --moltenvk) MVK="$2"; shift 2 ;;
        --bundle-id) BUNDLE_ID="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --build) BUILD="$2"; shift 2 ;;
        --steam-appid) STEAM_APPID="$2"; shift 2 ;;
        --with-test-content) TEST_CONTENT=1; shift ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
done
if [ -z "$MVK" ]; then
    for c in "${VULKAN_SDK:-/nonexistent}/macOS/lib/libMoltenVK.dylib" "${VULKAN_SDK:-/nonexistent}/lib/libMoltenVK.dylib" \
             "$(brew --prefix molten-vk 2> /dev/null || echo /nonexistent)/lib/libMoltenVK.dylib" /usr/local/lib/libMoltenVK.dylib; do
        [ -f "$c" ] && { MVK="$c"; break; }
    done
fi
[ -f "$BIN/deadground" ] || { echo "no $BIN/deadground"; exit 1; }
[ -n "$MVK" ] && [ -f "$MVK" ] || { echo "libMoltenVK.dylib not found (--moltenvk or VULKAN_SDK)"; exit 1; }

APP="$OUT/Deadground.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Frameworks"
cp "$BIN/deadground" "$APP/Contents/MacOS/deadground"
for d in shaders assets data web; do
    [ -d "$BIN/$d" ] && cp -R "$BIN/$d" "$APP/Contents/Resources/"
done
rm -rf "$APP/Contents/Resources/assets/cache"   # per-user, rebuilt into ~/Library/Application Support
if [ "$TEST_CONTENT" = 0 ]; then
    rm -f "$APP/Contents/Resources/assets/vgeo/rock_field.vgeo" "$APP/Contents/Resources/assets/vgeo/statue.vgeo" "$APP/Contents/Resources/assets/vgeo/building.vgeo"
fi
cp "$MVK" "$APP/Contents/Frameworks/libMoltenVK.dylib"
install_name_tool -id "@rpath/libMoltenVK.dylib" "$APP/Contents/Frameworks/libMoltenVK.dylib"

if [ -f "$BIN/libsteam_api.dylib" ]; then
    cp "$BIN/libsteam_api.dylib" "$APP/Contents/Frameworks/"
    # Valve's install name is @loader_path/libsteam_api.dylib (= Contents/MacOS): point it at Frameworks
    old="$(otool -L "$APP/Contents/MacOS/deadground" | awk '/libsteam_api/ {print $1}')"
    [ -n "$old" ] && install_name_tool -change "$old" "@rpath/libsteam_api.dylib" "$APP/Contents/MacOS/deadground"
    # steam_appid.txt only for runs outside Steam (dev / QA); Steam launches carry the app id
    [ -n "$STEAM_APPID" ] && echo "$STEAM_APPID" > "$APP/Contents/MacOS/steam_appid.txt"
fi
otool -l "$APP/Contents/MacOS/deadground" | grep -q "@executable_path/../Frameworks" ||
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$APP/Contents/MacOS/deadground"

ICON="$ROOT/tools/macos/AppIcon.png"
if [ -f "$ICON" ] && command -v iconutil > /dev/null; then
    set_dir="$(mktemp -d)/Deadground.iconset"
    mkdir -p "$set_dir"
    for s in 16 32 128 256 512; do
        sips -z $s $s "$ICON" --out "$set_dir/icon_${s}x${s}.png" > /dev/null
        sips -z $((s * 2)) $((s * 2)) "$ICON" --out "$set_dir/icon_${s}x${s}@2x.png" > /dev/null
    done
    iconutil -c icns "$set_dir" -o "$APP/Contents/Resources/Deadground.icns"
fi
sed -e "s/@BUNDLE_ID@/$BUNDLE_ID/" -e "s/@VERSION@/$VERSION/" -e "s/@BUILD@/$BUILD/" \
    "$ROOT/tools/macos/Info.plist.in" > "$APP/Contents/Info.plist"
plutil -lint "$APP/Contents/Info.plist" > /dev/null
echo "bundle: $APP ($(lipo -archs "$APP/Contents/MacOS/deadground"), MoltenVK $(lipo -archs "$APP/Contents/Frameworks/libMoltenVK.dylib"), $(du -sh "$APP" | cut -f1))"

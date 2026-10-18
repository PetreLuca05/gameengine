#!/usr/bin/env bash
# Code signing (docs/PLATFORM_MACOS.md "Signing / notarization"): the bundled dylibs first, then the
# app with the hardened runtime and tools/macos/Deadground.entitlements.
#   identity "-" (default): ad-hoc, for development and CI (runs on this Mac / after right-click Open)
#   DG_SIGN_IDENTITY="Developer ID Application: Name (TEAMID)": distribution, then tools/macos/notarize.sh
# usage: tools/macos/sign.sh <Deadground.app>
set -euo pipefail
APP="$1"
ID="${DG_SIGN_IDENTITY:--}"
ENT="$(cd "$(dirname "$0")" && pwd)/Deadground.entitlements"
OPTS=(--force --timestamp --options runtime)
[ "$ID" = "-" ] && OPTS=(--force --options runtime)   # ad-hoc: no timestamp server
for lib in "$APP"/Contents/Frameworks/*.dylib; do
    [ -f "$lib" ] && codesign "${OPTS[@]}" --sign "$ID" "$lib"
done
codesign "${OPTS[@]}" --entitlements "$ENT" --sign "$ID" "$APP"
codesign --verify --strict --verbose=2 "$APP"
echo "signed: $APP ($ID)"

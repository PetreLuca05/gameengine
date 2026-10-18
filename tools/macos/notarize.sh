#!/usr/bin/env bash
# Notarization + stapling of a Developer ID signed Deadground.app (docs/PLATFORM_MACOS.md):
# one-time setup of the keychain profile:
#   xcrun notarytool store-credentials deadground-notary --apple-id <id> --team-id <TEAMID> --password <app-specific>
# usage: DG_NOTARY_PROFILE=deadground-notary tools/macos/notarize.sh <Deadground.app>
set -euo pipefail
APP="$1"
PROFILE="${DG_NOTARY_PROFILE:-deadground-notary}"
ZIP="$(mktemp -d)/Deadground.zip"
ditto -c -k --keepParent "$APP" "$ZIP"
xcrun notarytool submit "$ZIP" --keychain-profile "$PROFILE" --wait
xcrun stapler staple "$APP"
spctl --assess --type execute --verbose=2 "$APP"
echo "notarized: $APP"

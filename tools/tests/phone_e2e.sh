#!/bin/sh
# Phone companion end-to-end: the game (phone_e2e.cfg) + headless Chromium (phone_e2e.mjs).
# Run from the build output directory (build/bin). Needs Node + an existing Playwright module
# (PLAYWRIGHT_PATH) + Chromium (CHROMIUM); the test installs nothing.
LOG=${LOG:-phone_e2e.log}
rm -f "$LOG"
EXE=./deadground; [ -x ./deadground.exe ] && EXE=./deadground.exe
"$EXE" --test --nosound --log "$LOG" +set net_port ${NET_PORT:-27394} +set phone_port ${PHONE_PORT:-47820} --script ../../tools/tests/phone_e2e.cfg &
GAME=$!
node ../../tools/tests/phone_e2e.mjs "$LOG" "${SHOTS:-.}"
RC=$?
kill $GAME 2>/dev/null
wait $GAME 2>/dev/null
grep -E "phone: (photo|video)|phone_stream_stats" "$LOG"
exit $RC

#!/bin/sh
# Checks mm_nav.cfg's log: the tab after every step (keys, pad, mouse, Back) and the quit prompt.
LOG=${1:-mm_nav.log}
EXPECT="Play Player Cosmetics Maps Maps Settings Credits Stats Play Play Play Play Player"
GOT=$(grep -o "mm_dump: active 1 tab [A-Za-z]*" "$LOG" | awk '{print $5}' | tr '\n' ' ' | sed 's/ $//')
PAGES=$(grep -o "page [a-z]*" "$LOG" | awk '{print $2}' | tr '\n' ' ')
echo "tabs:  $GOT"
echo "pages: $PAGES"
grep -q "season 3 hour 21 weather 'snow'" "$LOG" || { echo "FAIL: mm_map winter selection"; exit 1; }
grep -q "stack: mainmenu confirm" "$LOG" || { echo "FAIL: Back on Play did not open the quit prompt"; exit 1; }
[ "$GOT" = "$EXPECT" ] || { echo "FAIL: expected tabs: $EXPECT"; exit 1; }
echo "mm_nav: OK"

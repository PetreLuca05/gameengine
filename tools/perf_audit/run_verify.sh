#!/bin/sh
# Close a finding (docs/PERF_AUDIT.md "Closing a finding"): A (perf_fix_disable <id>) / B passes of the
# fix's scenes, then the timing / image / state verdict recorded in docs/perf/fixes.json + FINDINGS.md.
#   tools/perf_audit/run_verify.sh PF-0001 [scenes (default: the fix's scenes)] [frames=240] [reps=2]
# Env: as run_audit.sh; PERF_TARGET=1 marks this machine as the target hardware (a pass closes the finding).
set -e
FIX=$1
[ -z "$FIX" ] && { echo "usage: run_verify.sh PF-xxxx [scenes] [frames] [reps]"; exit 2; }
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PY=$(command -v python3 || command -v python)
SCENES=${2:-$("$PY" -c "import json,sys;print(','.join(json.load(open(sys.argv[1]))[sys.argv[2]]['scenes']))" "$ROOT/docs/perf/fixes.json" "$FIX")}
FRAMES=${3:-240}
REPS=${4:-2}
BIN="$ROOT/${DG_BUILD:-build}/bin"
EXE=${DG_EXE:-$( [ -x "$BIN/deadground.exe" ] && echo ./deadground.exe || echo ./deadground )}
OUT=${PERF_OUT:-$ROOT/${DG_BUILD:-build}/perf_out/verify}
SETTLE=${PERF_SETTLE:-120}
mkdir -p "$OUT/$FIX"
cd "$BIN"
printf 'perf_verify %s %s %s %s reps=%s settle=%s quit\n' "$FIX" "$SCENES" "$FRAMES" "$OUT" "$REPS" "$SETTLE" > "$OUT/$FIX/verify.cfg"
echo "perf_verify $FIX: $SCENES, $FRAMES frames, $REPS x A/B -> $OUT/$FIX"
$DG_DISPLAY_WRAP $EXE --test --nosound --log "$OUT/$FIX/verify.log" --script "$OUT/$FIX/verify.cfg" > /dev/null 2>&1 || echo "perf_verify: the game exited with $?"
"$PY" "$ROOT/tools/perf_audit/perf_audit.py" verify "$OUT/$FIX" --update ${PERF_TARGET:+--target}

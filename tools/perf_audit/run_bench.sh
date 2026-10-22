#!/bin/sh
# The FPS benchmark (docs/PERF_AUDIT.md "FPS benchmark"): the devroom_vegetation tour (perf_bench: every
# scene of scenes.json's "bench" list on one map load, the sim in real time, uncapped), then the report
# against the reference machine's baseline (docs/perf/bench_baseline.json) -> <out>/bench_report.md.
#   tools/perf_audit/run_bench.sh [levels=modern,low] [frames=600] [--write-baseline | --gate]
# A --test run has the default settings (no config); for the player's own settings run `perf_bench` from the
# console in the game instead and then perf_audit.py bench on its folder.
# Env: DG_BUILD (default build), DG_EXE, BENCH_OUT (default <build>/perf_out/bench_<date>_<time>),
#      DG_DISPLAY_WRAP (e.g. "xvfb-run -a" on Linux).
set -e
LEVELS=${1:-modern,low}
FRAMES=${2:-600}
MODE=$3
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/${DG_BUILD:-build}/bin"
EXE=${DG_EXE:-$( [ -x "$BIN/deadground.exe" ] && echo ./deadground.exe || echo ./deadground )}
OUT=${BENCH_OUT:-$ROOT/${DG_BUILD:-build}/perf_out/bench_$(date +%Y-%m-%d_%H%M)}
PY=$(command -v python3 || command -v python)
command -v cygpath > /dev/null 2>&1 && OUT=$(cygpath -m "$OUT")
mkdir -p "$OUT"
cd "$BIN"
echo "perf_bench: levels $LEVELS, $FRAMES frames per scene -> $OUT"
printf 'perf_bench frames=%s levels=%s out=%s quit\n' "$FRAMES" "$LEVELS" "$OUT" > "$OUT/bench.cfg"
$DG_DISPLAY_WRAP $EXE --test --nosound --log "$OUT/bench.log" --script "$OUT/bench.cfg" > /dev/null 2>&1 || echo "perf_bench: the game exited with $? (see $OUT/bench.log)"
grep "perf_bench:" "$OUT/bench.log" || true
"$PY" "$ROOT/tools/perf_audit/perf_audit.py" bench "$OUT" $MODE

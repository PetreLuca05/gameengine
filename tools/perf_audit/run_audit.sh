#!/bin/sh
# Performance audit (docs/PERF_AUDIT.md): every benchmark scene at Modern and Low, the 2-client LAN
# scene, then the analyzer -> docs/perf/AUDIT_<date>.md, .json and docs/perf/timeline/*.html.
#   tools/perf_audit/run_audit.sh [frames=300] [scenes=all]
# Env: DG_BUILD=build-alt (default build), DG_EXE (default deadground.exe / deadground), DG_PORT,
#      PERF_OUT (default ../perf_out/<date>, i.e. <repo>/<build>/perf_out/<date>), PERF_NOTE (a line for the report),
#      PERF_SETTLE (settle frames, default 120), DG_DISPLAY_WRAP (e.g. "xvfb-run -a" on Linux).
set -e
FRAMES=${1:-300}
SCENES=${2:-all}
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/${DG_BUILD:-build}/bin"
EXE=${DG_EXE:-$( [ -x "$BIN/deadground.exe" ] && echo ./deadground.exe || echo ./deadground )}
DATE=$(date +%Y-%m-%d)
OUT=${PERF_OUT:-$ROOT/${DG_BUILD:-build}/perf_out/$DATE}
SETTLE=${PERF_SETTLE:-120}
PY=$(command -v python3 || command -v python)
PORTARG=; [ -n "$DG_PORT" ] && PORTARG="+set net_port $DG_PORT"
mkdir -p "$OUT/lan2"
cd "$BIN"

echo "perf_audit: scenes $SCENES, $FRAMES frames, settle $SETTLE -> $OUT"
printf 'perf_audit %s %s %s levels=modern,low settle=%s refs quit\n' "$SCENES" "$FRAMES" "$OUT" "$SETTLE" > "$OUT/audit.cfg"
$DG_DISPLAY_WRAP $EXE --test --nosound --log "$OUT/audit.log" $PORTARG --script "$OUT/audit.cfg" > /dev/null 2>&1 || echo "perf_audit: the game exited with $? (see $OUT/audit.log)"

if [ "$SCENES" = "all" ]; then
  # 2-client LAN: the host runs the horde scene, a client joins; both capture the same window.
  sed "s|@OUT@|$OUT/lan2|g; s|@FRAMES@|$FRAMES|g" "$ROOT/tools/perf_audit/lan2_host.cfg" > "$OUT/lan2/host.cfg"
  sed "s|@OUT@|$OUT/lan2|g; s|@FRAMES@|$FRAMES|g" "$ROOT/tools/perf_audit/lan2_client.cfg" > "$OUT/lan2/client.cfg"
  $DG_DISPLAY_WRAP $EXE --test --nosound --log "$OUT/lan2/host.log" $PORTARG --script "$OUT/lan2/host.cfg" > /dev/null 2>&1 &
  HOST=$!
  sleep 6
  $DG_DISPLAY_WRAP $EXE --test --nosound --log "$OUT/lan2/client.log" $PORTARG +connect 127.0.0.1 --script "$OUT/lan2/client.cfg" > /dev/null 2>&1 || true
  wait $HOST || true
fi

"$PY" "$ROOT/tools/perf_audit/perf_audit.py" analyze "$OUT" --out "$ROOT/docs/perf" ${PERF_NOTE:+--note "$PERF_NOTE"}

#!/bin/sh
# The first audit on the target machine (docs/PERF_AUDIT.md "First run"): the full audit, then the A/B
# verification of every implemented fix (docs/perf/fixes.json). Afterwards commit docs/perf/.
#   tools/perf_audit/first_run.sh            (about 1.5 h on a desktop GPU at the default sizes)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
PERF_NOTE="First run on the target machine." "$HERE/run_audit.sh" "${FRAMES:-300}" all
for FIX in PF-0001 PF-0002 PF-0005; do
  PERF_TARGET=1 "$HERE/run_verify.sh" "$FIX" || echo "$FIX: not closed (see docs/perf/FINDINGS.md)"
done

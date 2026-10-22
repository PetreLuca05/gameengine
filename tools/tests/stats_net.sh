#!/bin/sh
# Player statistics over LAN (docs/STATS.md "Tests"): the host's combat deltas reach the client
# (shots, hits, a down, a death), public stats both ways. DG_BUILD (build dir), DG_EXE (deadground.exe;
# deadground on Linux), DG_PORT (another LAN port).
cd "$(dirname "$0")/../../${DG_BUILD:-build}/bin"
EXE=${DG_EXE:-./deadground.exe}
PORTARG=; [ -n "$DG_PORT" ] && PORTARG="+set net_port $DG_PORT"
$EXE --test --nosound --log ../st_host.log $PORTARG --script ../../tools/tests/stats_net_host.cfg > /dev/null 2>&1 &
sleep 6
$EXE --test --nosound --log ../st_client.log $PORTARG +set cl_name Alex +connect 127.0.0.1 --script ../../tools/tests/stats_net_client.cfg > /dev/null 2>&1 &
wait
grep -h "stats: profile\|stats: matches\|stats:   \|stats_public\|stats: match" ../st_host.log ../st_client.log
FAIL=0
grep -q "stats:   rifle .* shots [1-9]" ../st_client.log || { echo "FAIL: the client's shots did not arrive"; FAIL=1; }
grep "stats: matches" ../st_client.log | grep -q "downed 1" || { echo "FAIL: the client's down did not arrive"; FAIL=1; }
grep "stats: profile" ../st_client.log | grep -q "deaths 1" || { echo "FAIL: the client's death did not arrive"; FAIL=1; }
grep -q "stats_public 1: Alex" ../st_host.log || { echo "FAIL: the host has no public stats of the client"; FAIL=1; }
grep -q "stats_public 0: " ../st_client.log || { echo "FAIL: the client got no public stats of the host"; FAIL=1; }
[ $FAIL = 0 ] && echo "stats_net: OK"
exit $FAIL

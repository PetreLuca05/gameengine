#!/bin/sh
# Spectating LAN test (docs/SPECTATE.md "Tests"): host + target + observer with net_fakelag 80 and
# net_fakeloss 3 on every machine. The observer dies, spectates the target in first person through
# ADS, a magazine dump, reload / use rings, the sniper scope, the phone and driving. Then:
#   - spec_compare.py: the observer's replica vs the target's own state, tick by tick (spec_dump)
#   - spec_stats: measured delay (same clock) vs 2 x fakelag + 1 tick, corrections, stalls
#   - screenshots ../spec_t_*.png (target) vs ../spec_o_*.png (observer)
# DG_BUILD=build-alt for the alt build. DG_PORT=27115: another LAN port (the default 27015 may be held
# by another game / test on this machine).
cd "$(dirname "$0")/../../${DG_BUILD:-build}/bin"
PORTARG=; [ -n "$DG_PORT" ] && PORTARG="+set net_port $DG_PORT"
./deadground.exe --test --nosound --log ../spec_host.log $PORTARG --script ../../tools/tests/spec_host.cfg > /dev/null 2>&1 &
sleep 4
./deadground.exe --test --nosound --log ../spec_target.log $PORTARG +connect 127.0.0.1 --script ../../tools/tests/spec_target.cfg > /dev/null 2>&1 &
sleep 2
./deadground.exe --test --nosound --log ../spec_observer.log $PORTARG +connect 127.0.0.1 --script ../../tools/tests/spec_observer.cfg > /dev/null 2>&1
wait
grep -h "spectate:\|Spectate:\|net_snapstats" ../spec_host.log
grep -h "spec_stats" ../spec_observer.log | tail -5
python ../../tools/tests/spec_compare.py ../spec_target.log ../spec_observer.log --lag 80

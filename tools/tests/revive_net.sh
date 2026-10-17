#!/bin/sh
# Revive loop over LAN (docs/REVIVE.md "Tests"): host + client + a late joiner during the reconstruction.
# DG_BUILD (build dir), DG_EXE (deadground.exe; deadground on Linux), SHOTDIR (screenshots).
cd "$(dirname "$0")/../../${DG_BUILD:-build}/bin"
EXE=${DG_EXE:-./deadground.exe}
SHOT=${SHOTDIR:-..}
for c in host client late; do sed "s|SHOTDIR|$SHOT|g" ../../tools/tests/revive_net_$c.cfg > ../revive_net_$c.run.cfg; done
$EXE --test --nosound --log ../rv_host.log --script ../revive_net_host.run.cfg > /dev/null 2>&1 &
sleep 6
$EXE --test --nosound --log ../rv_client.log +set cl_name Alex +connect 127.0.0.1 --script ../revive_net_client.run.cfg > /dev/null 2>&1 &
# The late joiner connects once the host reports the reconstruction.
i=0
while ! grep -q "state reconstructing" ../rv_host.log 2>/dev/null && [ $i -lt 1500 ]; do sleep 1; i=$((i+1)); done
$EXE --test --nosound --log ../rv_late.log +set cl_name Kim +connect 127.0.0.1 --script ../revive_net_late.run.cfg > /dev/null 2>&1 &
wait
grep -h "revive_dump\|revive_wait\|death_impact\|death_cut\|revive:" ../rv_host.log ../rv_client.log ../rv_late.log

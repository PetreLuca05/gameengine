#!/bin/sh
# Two clients looting one crate over LAN with 150 ms fake lag (docs/INVENTORY.md "Networking").
# DG_BUILD (build dir), DG_EXE (deadground.exe; deadground on Linux), SHOTDIR (screenshots),
# DG_PORT (another LAN port). The scripts wait on states (net_wait_clients, inv_wait), not frame counts.
# Then: python3 tools/tests/inv_net_check.py <build>/ (run at the end).
cd "$(dirname "$0")/../../${DG_BUILD:-build}/bin"
EXE=${DG_EXE:-./deadground.exe}
SHOT=${SHOTDIR:-..}
PORTARG=; [ -n "$DG_PORT" ] && PORTARG="+set net_port $DG_PORT"
sed "s|SHOTDIR|$SHOT|g" ../../tools/tests/inv_net_host.cfg > ../inv_net_host.run.cfg
for c in a b; do sed "s|SHOTDIR|$SHOT|g; s|CLIENT|$c|g" ../../tools/tests/inv_net_client.cfg > ../inv_net_$c.run.cfg; done
rm -f ../inv_host.log ../inv_a.log ../inv_b.log
$EXE --test --nosound --log ../inv_host.log $PORTARG --script ../inv_net_host.run.cfg > /dev/null 2>&1 &
HOST=$!
# Startup waits on the logs, not on a fixed sleep (a loaded machine took two minutes to listen): the
# clients start once the host listens, client B once A has joined.
wait_log() {   # <log> <text> <seconds>: 0 when the line is there; 1 on timeout or when the host is gone
    n=0
    while [ $n -lt "$3" ]; do
        grep -q "$2" "$1" 2>/dev/null && return 0
        kill -0 $HOST 2>/dev/null || return 1
        sleep 1
        n=$((n + 1))
    done
    return 1
}
wait_log ../inv_host.log "lan: listening" 600 || echo "inv_net: the host never listened (see inv_host.log)"
$EXE --test --nosound --log ../inv_a.log $PORTARG +set cl_name LooterA +connect 127.0.0.1 --script ../inv_net_a.run.cfg > /dev/null 2>&1 &
wait_log ../inv_a.log "client: joined" 600 || echo "inv_net: client A never joined (see inv_a.log)"
$EXE --test --nosound --log ../inv_b.log $PORTARG +set cl_name LooterB +connect 127.0.0.1 --script ../inv_net_b.run.cfg > /dev/null 2>&1 &
wait
# python3 (Linux) or python (Windows: python3 is only the Store alias there)
PY=python3; python3 -c '' 2>/dev/null || PY=python
$PY ../../tools/tests/inv_net_check.py ..

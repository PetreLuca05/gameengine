#!/bin/sh
# Two clients looting one crate over LAN with 150 ms fake lag (docs/INVENTORY.md "Networking").
# DG_BUILD (build dir), DG_EXE (deadground.exe; deadground on Linux), SHOTDIR (screenshots),
# DG_PORT (another LAN port). Then: python3 tools/tests/inv_net_check.py <build>/
cd "$(dirname "$0")/../../${DG_BUILD:-build}/bin"
EXE=${DG_EXE:-./deadground.exe}
SHOT=${SHOTDIR:-..}
PORTARG=; [ -n "$DG_PORT" ] && PORTARG="+set net_port $DG_PORT"
sed "s|SHOTDIR|$SHOT|g" ../../tools/tests/inv_net_host.cfg > ../inv_net_host.run.cfg
for c in a b; do sed "s|SHOTDIR|$SHOT|g; s|CLIENT|$c|g" ../../tools/tests/inv_net_client.cfg > ../inv_net_$c.run.cfg; done
$EXE --test --nosound --log ../inv_host.log $PORTARG --script ../inv_net_host.run.cfg > /dev/null 2>&1 &
sleep 6
$EXE --test --nosound --log ../inv_a.log $PORTARG +set cl_name LooterA +connect 127.0.0.1 --script ../inv_net_a.run.cfg > /dev/null 2>&1 &
sleep 1
$EXE --test --nosound --log ../inv_b.log $PORTARG +set cl_name LooterB +connect 127.0.0.1 --script ../inv_net_b.run.cfg > /dev/null 2>&1 &
wait
python3 ../../tools/tests/inv_net_check.py ..

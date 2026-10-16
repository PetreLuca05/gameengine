#!/bin/sh
# Two-instance open-world test (docs/ZOMBIE_MAP.md): a host on zombieland and a client that joins it.
# Port: ZL_PORT (default 27031, so a running game on 27015 does not interfere).
port=${ZL_PORT:-27031}
cd "$(dirname "$0")/../../${DG_BUILD:-build}/bin"
./deadground.exe --test --nosound --log ../zl_host.log +set net_port $port --script ../../tools/tests/zl_net_host.cfg > /dev/null 2>&1 &
sleep 12
./deadground.exe --test --nosound --log ../zl_client.log +set net_port $port +connect 127.0.0.1:$port --script ../../tools/tests/zl_net_client.cfg > /dev/null 2>&1
wait
grep -h "openworld:\|level 'Zombieland'\|connected\|welcome\|pos -\?[0-9]\|at (" ../zl_host.log ../zl_client.log

#!/bin/sh
# Two-instance night vision test (docs/NIGHT_OPTICS.md "Networking"): the host puts its goggles down with
# the IR illuminator, the client sees the replicated EF_NVG / EF_NVG_IR (the goggles on the host's head,
# its illuminator in its own NVG) and screenshots the host; then the host switches them off.
#   tools/tests/nvg_net.sh && python3 tools/tests/nvg_net_check.py build/nvg_client.log
cd "$(dirname "$0")/../../build/bin"
PORT=${PORT:-28640}
EXE=./deadground.exe; [ -x ./deadground ] && EXE=./deadground   # macOS / Linux build
$EXE --test --nosound --log ../nvg_host.log +set net_port $PORT --script ../../tools/tests/nvg_net_host.cfg > /dev/null 2>&1 &
sleep 5
$EXE --test --nosound --log ../nvg_client.log +set net_port $PORT +connect 127.0.0.1 --script ../../tools/tests/nvg_net_client.cfg > /dev/null 2>&1
wait
grep -h "nvg_report\|connected\|protocol" ../nvg_host.log ../nvg_client.log

#!/bin/sh
# Phone LAN check (docs/PHONE.md "Tests"): a host holding its phone, a client looking at it. The
# client sees the replicated pose (the phone raised in portrait) and a dark model screen; the host's
# screen content is only on the host's HUD panel. Screenshots ../phone_net_host.png / _client.png.
# DG_PORT: the LAN port (default 27395, the user's game may hold 27015).
cd "$(dirname "$0")/../../${DG_BUILD:-build}/bin"
PORT=${DG_PORT:-27395}
EXE=./deadground.exe; [ -x ./deadground ] && EXE=./deadground   # macOS / Linux build
$EXE --test --nosound --log ../phone_net_host.log +set net_port $PORT +set phone_port 47395 --script ../../tools/tests/phone_net_host.cfg > /dev/null 2>&1 &
sleep 4
$EXE --test --nosound --log ../phone_net_client.log +set net_port $PORT +set phone_port 47396 +connect 127.0.0.1 --script ../../tools/tests/phone_net_client.cfg > /dev/null 2>&1
wait
grep -h "phone_status\|connected\|protocol" ../phone_net_host.log ../phone_net_client.log | head -20

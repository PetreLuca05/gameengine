#!/bin/sh
# Ballistics LAN test (docs/BALLISTICS.md "Tests"): host + client. The client's predicted pistol
# hits ("predict shot ...") must match the host's authoritative ones ("host shot ...") within
# 0.25 m; the host's rifle burst is replayed on the client ("replay shot ...") with cracks / reports
# scheduled for its listener (snd_trace). DG_BUILD=build-alt for the alt build.
cd "$(dirname "$0")/../../${DG_BUILD:-build}/bin"
EXE=./deadground.exe; [ -x ./deadground ] && EXE=./deadground
$EXE --test --nosound --log ../bal_net_host.log --script ../../tools/tests/bal_net_host.cfg > /dev/null 2>&1 &
sleep 4
$EXE --test --nosound --log ../bal_net_client.log +connect 127.0.0.1 --script ../../tools/tests/bal_net_client.cfg > /dev/null 2>&1
wait
python3 ../../tools/tests/bal_net_compare.py ../bal_net_host.log ../bal_net_client.log

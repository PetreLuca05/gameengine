#!/bin/sh
# Two-instance ledge hang test: the host hangs (rifle slung, pistol aimed / fired, climb up), the
# client walks up to the wall and screenshots the remote pose (t_hang_c*.png).
cd "$(dirname "$0")/../../build/bin"
PORT=${PORT:-28610}
./deadground.exe --test --nosound --log ../hang_host.log +set net_port $PORT --script ../../tools/tests/hang_net_host.cfg > /dev/null 2>&1 &
sleep 5
./deadground.exe --test --nosound --log ../hang_client.log +set net_port $PORT +connect 127.0.0.1 --script ../../tools/tests/hang_net_client.cfg > /dev/null 2>&1
wait
grep -h "pos \|connected\|protocol" ../hang_host.log ../hang_client.log

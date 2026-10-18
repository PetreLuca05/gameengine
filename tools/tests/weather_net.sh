#!/bin/sh
# Two-instance weather replication test: host changes the weather, a client joins 6 s later
# (late join), both dump the evaluated weather at fixed server ticks.
cd "$(dirname "$0")/../../build/bin"
EXE=./deadground.exe; [ -x ./deadground ] && EXE=./deadground   # macOS / Linux build
$EXE --test --nosound --log ../wx_host.log --script ../../tools/tests/weather_net_host.cfg > /dev/null 2>&1 &
sleep 6
$EXE --test --nosound --log ../wx_client.log +connect 127.0.0.1 --script ../../tools/tests/weather_net_client.cfg > /dev/null 2>&1
wait
grep -h "weather_test dump\|weather v" ../wx_host.log ../wx_client.log

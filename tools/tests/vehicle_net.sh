#!/bin/sh
# Two-instance vehicle test: host + a client that drives with 80 ms fake lag (prediction).
cd "$(dirname "$0")/../../${DG_BUILD:-build}/bin"
./deadground.exe --test --nosound --log ../veh_host.log --script ../../tools/tests/vehicle_net_host.cfg > /dev/null 2>&1 &
sleep 4
./deadground.exe --test --nosound --log ../veh_client.log +connect 127.0.0.1 --script ../../tools/tests/vehicle_net_client.cfg > /dev/null 2>&1
wait
grep -h "vehicle_dump\|predicting\|out at\|-> seat" ../veh_host.log ../veh_client.log

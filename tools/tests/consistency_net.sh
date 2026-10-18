#!/bin/sh
# Join handshake (docs/SETTINGS_AUDIT.md): a host, then a client with the same consistency set
# (must be welcomed), then a client whose dev-build console changed a locked value (r_gamma 1.5:
# must be rejected with "consistency set mismatch"). The host's server rule (r_foliage_density 0.6)
# must reach the welcomed client and go away when it leaves. Run from build/bin.
# DG_PORT=27115: another LAN port. Logs: consistency_host.log / _ok.log / _bad.log.
EXE=./deadground; [ -x ./deadground.exe ] && EXE=./deadground.exe
T=${T:-../../tools/tests}
PORT=${DG_PORT:-27015}
"$EXE" --test --nosound --log consistency_host.log +set net_port $PORT --script $T/consistency_host.cfg > /dev/null 2>&1 &
HOST=$!
sleep ${CLIENT_DELAY:-20}
"$EXE" --test --nosound --log consistency_ok.log +set net_port $((PORT + 1)) +connect 127.0.0.1:$PORT --script $T/consistency_client.cfg > /dev/null 2>&1
"$EXE" --test --nosound --log consistency_bad.log +set net_port $((PORT + 2)) +set r_gamma 1.5 +connect 127.0.0.1:$PORT --script $T/consistency_client.cfg > /dev/null 2>&1
wait $HOST
fail() { echo "FAIL: $1"; exit 1; }
grep -h "net: \|consistency" consistency_host.log | sed 's/^\[ *[0-9.]*\] //' | head -20
grep -q "rejecting .*consistency set mismatch" consistency_host.log || fail "the edited client was not rejected"
[ "$(grep -c 'rejecting' consistency_host.log)" = "1" ] || fail "the matching client was rejected too"
grep -q "consistency set mismatch" consistency_bad.log || fail "the edited client did not get the reason"
[ "$(grep -o 'foliage_density=[0-9.]*' consistency_ok.log | head -1)" = "foliage_density=0.6" ] || fail "the host's foliage rule did not reach the client"
[ "$(grep -o 'foliage_density=[0-9.]*' consistency_ok.log | tail -1)" = "foliage_density=1" ] || fail "the client did not get its own foliage density back after leaving"
echo "consistency_net: OK"

#!/bin/sh
# Two instances on one machine over the LAN transport (docs/MENU_LOBBY.md "Verification").
#   lobby_lan.sh [flow|kick|migrate]     (default flow)
# Run from the build output directory (build/bin). Logs: lobby_<mode>_host.log / _client.log.
MODE=${1:-flow}
EXE=./deadground; [ -x ./deadground.exe ] && EXE=./deadground.exe
T=${T:-../../tools/tests}
case "$MODE" in
  flow) H=lobby_lan_host.cfg; C=lobby_lan_client.cfg ;;
  kick) H=lobby_kick_host.cfg; C=lobby_kick_client.cfg ;;
  migrate) H=lobby_migrate_host.cfg; C=lobby_migrate_client.cfg ;;
  *) echo "usage: lobby_lan.sh [flow|kick|migrate]"; exit 2 ;;
esac
"$EXE" --test --menu --nosound --log lobby_${MODE}_host.log --script $T/$H > /dev/null 2>&1 &
HOST=$!
sleep ${CLIENT_DELAY:-4}
"$EXE" --test --menu --nosound --log lobby_${MODE}_client.log --script $T/$C > /dev/null 2>&1
wait $HOST
H=lobby_${MODE}_host.log; C=lobby_${MODE}_client.log
grep -h "lobby: \|lobby_status\|^.*  #\|mm_dump\|season \|net: " $H $C | sed 's/^\[ *[0-9.]*\] //' | head -80
fail() { echo "FAIL: $1"; exit 1; }
case "$MODE" in
  flow)
    grep -q "joining -> \|joining + snapshot -> lobby" $C || fail "client never reached the lobby"
    grep -q "lobby chat: Client: hi host" $H || fail "client chat did not reach the host"
    grep -q "lobby chat: Host: hello from the host" $C || fail "host chat did not reach the client"
    grep -q "loading + load_done -> in_game" $H || fail "host did not reach the game"
    grep -q "loading + load_done -> in_game" $C || fail "client did not reach the game"
    grep -q "season autumn" $H || fail "season not applied on the host" ;;
  kick)
    grep -q "kicked -> closed" $C || fail "client not kicked"
    grep -q "You were kicked from this lobby" $C || fail "rejoin was not refused" ;;
  migrate)
    grep -q "You are the host now\|migrating + migrated -> lobby" $C || fail "client did not take over" ;;
esac
echo "lobby_lan $MODE: OK"

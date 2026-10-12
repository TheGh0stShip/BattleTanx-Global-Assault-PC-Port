#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
pc_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir=${1:-"$pc_root/build-live-test"}
api=${BTGA_MASTER_API:-https://btga.thegh0stship.com/api/v1}
binary="$build_dir/btga_pc_live_multiplayer_test"

command -v curl >/dev/null 2>&1 || { echo "curl is required" >&2; exit 2; }
command -v jq >/dev/null 2>&1 || { echo "jq is required" >&2; exit 2; }

cmake -S "$pc_root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target btga_pc_live_multiplayer_test

created=$(curl -fsS -X POST "$api/lobbies" -H 'content-type: application/json' --data \
    '{"name":"PC port live smoke test","protocol":1,"build":"pc-port-live-smoke-v1","platform":"pc","capabilities":2,"region":"eastus","max_players":4,"current_players":1}')
lobby=$(printf '%s' "$created" | jq -r .lobby.id)
secret=$(printf '%s' "$created" | jq -r .host_secret)
session=$(printf '%s' "$created" | jq -r .session_id)
relay=$(printf '%s' "$created" | jq -r .relay.address)
port=$(printf '%s' "$created" | jq -r .relay.udp_port)
host_token=$(printf '%s' "$created" | jq -r .relay_token)
host_log=$(mktemp)

cleanup() {
    [ -z "${host_pid:-}" ] || kill "$host_pid" 2>/dev/null || true
    curl -fsS -X DELETE "$api/lobbies/$lobby" \
        -H "authorization: Bearer $secret" >/dev/null 2>&1 || true
    rm -f "$host_log"
}
trap cleanup EXIT INT TERM

joined=$(curl -fsS -X POST "$api/lobbies/$lobby/join" \
    -H 'content-type: application/json' \
    --data '{"protocol":1,"build":"pc-port-live-smoke-v1","platform":"pc"}')
client_token=$(printf '%s' "$joined" | jq -r .join_token)

"$binary" host "$relay" "$port" "$session" "$host_token" >"$host_log" 2>&1 &
host_pid=$!
i=0
while ! grep -q '^BOUND 0$' "$host_log"; do
    kill -0 "$host_pid" 2>/dev/null || { cat "$host_log" >&2; exit 1; }
    i=$((i + 1)); [ "$i" -lt 200 ] || { echo "host bind timeout" >&2; exit 1; }
    sleep 0.05
done

"$binary" client "$relay" "$port" "$session" "$client_token"
wait "$host_pid"
host_pid=
grep -q '^RECEIVED 1$' "$host_log"
cat "$host_log"
echo "PC port public multiplayer smoke test passed via $relay:$port"

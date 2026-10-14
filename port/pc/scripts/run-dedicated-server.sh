#!/usr/bin/env sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
    echo "usage: $0 ROM [BATTLETANXGA_BINARY] [STATE_DIRECTORY]" >&2
    exit 2
fi

rom=$1
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
binary=${2:-"$repo_root/build-pc/BattleTanxGA"}
state=${3:-"$repo_root/build-pc/dedicated-state"}
max_failures=${BTGA_SERVER_MAX_FAILURES:-10}
stable_seconds=${BTGA_SERVER_STABLE_SECONDS:-300}
hang_seconds=${BTGA_SERVER_HANG_SECONDS:-60}
startup_health_seconds=${BTGA_SERVER_STARTUP_HEALTH_SECONDS:-120}

case "$rom" in
    *.z64) ;;
    *) echo "the dedicated server requires a local .z64 ROM" >&2; exit 2 ;;
esac
[ -f "$rom" ] || { echo "ROM not found: $rom" >&2; exit 2; }
[ -x "$binary" ] || { echo "server binary not executable: $binary" >&2; exit 2; }
case "$max_failures" in
    ''|*[!0-9]*) echo "BTGA_SERVER_MAX_FAILURES must be a positive integer" >&2; exit 2 ;;
esac
case "$stable_seconds" in
    ''|*[!0-9]*) echo "BTGA_SERVER_STABLE_SECONDS must be a positive integer" >&2; exit 2 ;;
esac
case "$hang_seconds" in
    ''|*[!0-9]*) echo "BTGA_SERVER_HANG_SECONDS must be a positive integer" >&2; exit 2 ;;
esac
case "$startup_health_seconds" in
    ''|*[!0-9]*) echo "BTGA_SERVER_STARTUP_HEALTH_SECONDS must be a positive integer" >&2; exit 2 ;;
esac
[ "$max_failures" -gt 0 ] && [ "$stable_seconds" -gt 0 ] || {
    echo "server limits must be greater than zero" >&2
    exit 2
}
[ "$hang_seconds" -gt 0 ] && [ "$startup_health_seconds" -gt 0 ] || {
    echo "health timeouts must be greater than zero" >&2
    exit 2
}

command -v flock >/dev/null 2>&1 || {
    echo "flock is required to protect dedicated authority state" >&2
    exit 2
}
command -v sha256sum >/dev/null 2>&1 || {
    echo "sha256sum is required to pin the authority binary and ROM" >&2
    exit 2
}
binary=$(CDPATH= cd -- "$(dirname -- "$binary")" && pwd)/$(basename -- "$binary")
rom=$(CDPATH= cd -- "$(dirname -- "$rom")" && pwd)/$(basename -- "$rom")
binary_hash=$(sha256sum -- "$binary" | awk '{print $1}')
rom_hash=$(sha256sum -- "$rom" | awk '{print $1}')
umask 077
mkdir -p "$state/logs"
state=$(CDPATH= cd -- "$state" && pwd)
logs=$state/logs
health=$state/authority-health.json
exec 9>"$state/supervisor.lock"
if ! flock -n 9; then
    echo "another dedicated authority supervisor owns state directory: $state" >&2
    exit 1
fi
identity_file=$state/runtime-identity.txt
identity="BTGA1:$binary_hash:$rom_hash"
if [ -f "$identity_file" ]; then
    IFS= read -r stored_identity < "$identity_file" || stored_identity=
    if [ "$stored_identity" != "$identity" ]; then
        echo "authority state belongs to a different executable or ROM; use a fresh state directory" >&2
        exit 1
    fi
else
    identity_tmp=$identity_file.tmp.$$
    printf '%s\n' "$identity" > "$identity_tmp"
    mv -- "$identity_tmp" "$identity_file"
fi
child=
cleanup() {
    if [ -n "$child" ] && kill -0 "$child" 2>/dev/null; then
        kill "$child" 2>/dev/null || true
        wait "$child" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

generation=0
failures=0
echo "Dedicated authority supervisor active."
echo "State: $state"
echo "Press Ctrl+C to stop. Existing clients may remain connected during recovery."

while :; do
    current_binary_hash=$(sha256sum -- "$binary" | awk '{print $1}')
    current_rom_hash=$(sha256sum -- "$rom" | awk '{print $1}')
    if [ "$current_binary_hash" != "$binary_hash" ]; then
        echo "authority binary changed while the supervisor was active; refusing to mix builds in one persistent session" >&2
        exit 1
    fi
    if [ "$current_rom_hash" != "$rom_hash" ]; then
        echo "authority ROM changed while the supervisor was active; refusing to mix ROMs in one persistent session" >&2
        exit 1
    fi
    generation=$((generation + 1))
    stamp=$(date -u +%Y%m%dT%H%M%SZ)
    prefix=authority-$stamp-$generation
    stdout=$logs/$prefix.stdout.log
    stderr=$logs/$prefix.stderr.log
    started=$(date +%s)
    published=0
    health_seen=0
    last_health_write=0

    BTGA_DEDICATED_STATE_DIR=$state \
        "$binary" --dedicated-server-public "$rom" >"$stdout" 2>"$stderr" &
    child=$!
    while kill -0 "$child" 2>/dev/null; do
        if [ "$published" -eq 0 ] && [ -s "$stdout" ]; then
            lobby=$(sed -n 's/^ONLINE_LOBBY_ID=//p' "$stdout" | head -n 1)
            if [ -n "$lobby" ]; then
                published=1
                echo "ONLINE_LOBBY_ID=$lobby"
                echo "Authority generation $generation is online."
            fi
        fi
        now=$(date +%s)
        if [ -f "$health" ]; then
            health_write=$(stat -c %Y "$health" 2>/dev/null || echo 0)
            if [ "$health_write" -ge "$started" ]; then
                health_seen=1
                last_health_write=$health_write
            fi
        fi
        if [ "$health_seen" -eq 1 ] &&
           [ $((now - last_health_write)) -gt "$hang_seconds" ]; then
            echo "authority generation $generation stopped advancing its main loop; terminating it for recovery" >&2
            kill "$child" 2>/dev/null || true
            break
        fi
        if [ "$health_seen" -eq 0 ] &&
           [ $((now - started)) -gt "$startup_health_seconds" ]; then
            echo "authority generation $generation never published main-loop health; terminating it for recovery" >&2
            kill "$child" 2>/dev/null || true
            break
        fi
        sleep 1
    done
    set +e
    wait "$child"
    exit_code=$?
    set -e
    child=
    runtime=$(( $(date +%s) - started ))
    if [ "$runtime" -ge "$stable_seconds" ]; then failures=0; fi
    failures=$((failures + 1))
    if [ "$failures" -gt "$max_failures" ]; then
        echo "authority exceeded $max_failures consecutive failures; last exit $exit_code; log: $stderr" >&2
        exit 1
    fi
    exponent=$((failures - 1))
    [ "$exponent" -le 5 ] || exponent=5
    delay=$((1 << exponent))
    [ "$delay" -le 30 ] || delay=30
    echo "authority exited with code $exit_code after ${runtime}s; restarting generation $((generation + 1)) in ${delay}s" >&2
    sleep "$delay"
done

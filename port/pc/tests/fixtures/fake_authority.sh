#!/usr/bin/env sh
set -eu

state=${BTGA_DEDICATED_STATE_DIR:?}
mode=${BTGA_FAKE_AUTHORITY_MODE:-crash}
marker=$state/fake-$mode-seen
if [ -f "$marker" ]; then exit 7; fi
touch "$marker"

case "$mode" in
    crash) exit 7 ;;
    mutate)
        # The supervisor test runs this mode from a disposable copy. Simulate a
        # build replacing the executable between authority generations.
        printf '\n# replaced while session is active\n' >> "$0"
        exit 7
        ;;
    stall|delete-health)
        printf '%s\n' '{"version":1,"loop_sequence":1,"next_tick":0,"connected_mask":0,"recovery_mask":0,"gameplay_tick_active":false}' > "$state/authority-health.json"
        if [ "$mode" = delete-health ]; then
            sleep 2
            rm -f "$state/authority-health.json"
        fi
        ;;
    no-health) ;;
    *) echo "unknown fake authority mode: $mode" >&2; exit 2 ;;
esac

while :; do sleep 60; done

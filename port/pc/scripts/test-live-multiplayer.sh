#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
pc_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir=${1:-"$pc_root/build-live-test"}
relay_runs=${2:-1}

case "$relay_runs" in
    ''|*[!0-9]*) echo "relay run count must be a positive integer" >&2; exit 2 ;;
    0) echo "relay run count must be greater than zero" >&2; exit 2 ;;
esac

cmake -S "$pc_root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target \
    btga_pc_tests btga_bootstrap_recovery_tests \
    btga_pc_synchronization_soak btga_authoritative_server_soak \
    btga_canonical_journal_soak btga_pc_native_online_e2e
ctest --test-dir "$build_dir" --output-on-failure

run=1
while [ "$run" -le "$relay_runs" ]; do
    printf 'public relay lifecycle %s/%s: ' "$run" "$relay_runs"
    "$build_dir/btga_pc_native_online_e2e"
    run=$((run + 1))
done

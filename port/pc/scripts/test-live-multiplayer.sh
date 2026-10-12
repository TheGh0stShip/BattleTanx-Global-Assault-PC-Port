#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
pc_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir=${1:-"$pc_root/build-live-test"}

cmake -S "$pc_root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target btga_pc_native_online_e2e
"$build_dir/btga_pc_native_online_e2e"

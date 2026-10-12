#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../../.." && pwd)"
matching="${BTGA_MATCHING_ROOT:-/home/steve/projects/VitaTanxGA}"
recomp="${N64RECOMP:-$root/.deps/N64Recomp/build/N64Recomp}"
rsp_recomp="${RSPRECOMP:-$root/.deps/N64Recomp/build/RSPRecomp}"

[[ -x "$recomp" ]] || {
    echo "N64Recomp not found at $recomp; build the official tool first" >&2
    exit 1
}
[[ -f "$matching/config/us/recomp_function_boundaries.toml" ]] || {
    echo "matching function catalogue missing in $matching" >&2
    exit 1
}
[[ -f "$matching/baseroms/us/baserom.z64" ]] || {
    echo "verified local ROM missing in $matching/baseroms/us" >&2
    exit 1
}

cd "$root/port/pc/recomp"
mkdir -p input generated
ln -sfn "$matching/config/us/recomp_function_boundaries.toml" input/functions.toml
ln -sfn "$matching/baseroms/us/baserom.z64" input/battletanx_ga.z64
rm -rf generated/functions generated/battletanx_ga.bin
"$recomp" btga.toml

if [[ -x "$rsp_recomp" ]]; then
    "$rsp_recomp" n_aspMain.toml
else
    echo "RSPRecomp not found at $rsp_recomp; skipped audio microcode generation" >&2
fi

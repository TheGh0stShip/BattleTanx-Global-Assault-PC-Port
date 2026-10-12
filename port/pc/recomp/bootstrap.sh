#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../../.." && pwd)"
source_dir="$root/.deps/N64Recomp"
commit=ffb39cdad1da5de07eaaa48bd1db4a89a7986771

if [[ ! -d "$source_dir/.git" ]]; then
    mkdir -p "$root/.deps"
    git clone --recursive https://github.com/N64Recomp/N64Recomp.git "$source_dir"
fi
git -C "$source_dir" fetch --depth 1 origin "$commit"
git -C "$source_dir" checkout --detach "$commit"
git -C "$source_dir" submodule update --init --recursive
cmake -S "$source_dir" -B "$source_dir/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$source_dir/build" --target N64RecompCLI RSPRecomp

#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../../.." && pwd)"
source_dir="$root/.deps/N64ModernRuntime"
commit=cdf5abbd5026fef5c364c676e4667c45e42b6863

if [[ ! -d "$source_dir/.git" ]]; then
    mkdir -p "$root/.deps"
    git clone https://github.com/N64Recomp/N64ModernRuntime.git "$source_dir"
fi
git -C "$source_dir" fetch --depth 1 origin "$commit"
git -C "$source_dir" checkout --detach "$commit"
git -C "$source_dir" submodule update --init --recursive

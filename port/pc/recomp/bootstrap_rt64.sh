#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../../.." && pwd)"
destination="${RT64_ROOT:-$root/.deps/rt64}"
revision="23cab603c4f9f4a8b369b38e036f1aa484603878"

if [[ ! -d "$destination/.git" ]]; then
    git clone https://github.com/rt64/rt64.git "$destination"
fi

git -C "$destination" fetch --depth=1 origin "$revision"
git -C "$destination" checkout --detach "$revision"
git -C "$destination" submodule update --init --recursive --depth=1

actual="$(git -C "$destination" rev-parse HEAD)"
[[ "$actual" == "$revision" ]] || {
    echo "RT64 revision mismatch: expected $revision, got $actual" >&2
    exit 1
}

echo "RT64 ready at $destination ($actual)"

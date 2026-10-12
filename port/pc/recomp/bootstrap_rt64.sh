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

nfd="$destination/src/contrib/nativefiledialog-extended"
patch="$root/port/pc/recomp/patches/rt64-nfd-stub.patch"
if git -C "$nfd" apply --check "$patch" 2>/dev/null; then
    git -C "$nfd" apply "$patch"
elif ! git -C "$nfd" apply --reverse --check "$patch" 2>/dev/null; then
    echo "RT64 file-dialog patch does not apply cleanly" >&2
    exit 1
fi

overlay_patch="$root/port/pc/recomp/patches/rt64-custom-overlay.patch"
if git -C "$destination" apply --check "$overlay_patch" 2>/dev/null; then
    git -C "$destination" apply "$overlay_patch"
elif ! git -C "$destination" apply --reverse --check "$overlay_patch" 2>/dev/null; then
    echo "RT64 custom-overlay patch does not apply cleanly" >&2
    exit 1
fi

actual="$(git -C "$destination" rev-parse HEAD)"
[[ "$actual" == "$revision" ]] || {
    echo "RT64 revision mismatch: expected $revision, got $actual" >&2
    exit 1
}

echo "RT64 ready at $destination ($actual)"

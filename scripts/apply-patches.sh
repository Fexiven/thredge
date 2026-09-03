#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Apply the patches in patches/ to third_party/esp-thread-br.
#
# The submodule tracks upstream main and Dependabot bumps it, so the patches
# live here rather than in a fork. Idempotent: already-applied patches are
# skipped, so it is safe to run before every build.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
submodule="$repo_root/third_party/esp-thread-br"

if [ ! -d "$submodule/.git" ] && [ ! -f "$submodule/.git" ]; then
    echo "error: $submodule is not checked out. Run:" >&2
    echo "    git submodule update --init --recursive" >&2
    exit 1
fi

shopt -s nullglob
patches=("$repo_root"/patches/*.patch)
if [ ${#patches[@]} -eq 0 ]; then
    echo "No patches to apply."
    exit 0
fi

for patch in "${patches[@]}"; do
    name="$(basename "$patch")"
    if git -C "$submodule" apply --reverse --check "$patch" >/dev/null 2>&1; then
        echo "already applied: $name"
    elif git -C "$submodule" apply --check "$patch" >/dev/null 2>&1; then
        git -C "$submodule" apply "$patch"
        echo "applied:         $name"
    else
        echo "error: $name does not apply cleanly to third_party/esp-thread-br." >&2
        echo "Upstream has probably moved. Rebase the patch and commit the result." >&2
        exit 1
    fi
done

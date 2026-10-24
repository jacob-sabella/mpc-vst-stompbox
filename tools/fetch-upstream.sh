#!/usr/bin/env bash
# Clones the guitarix and GxPlugins commits named in upstream.env into a folder (default build/upstream) and refuses
# a checkout whose HEAD is not that commit. GxPlugins' pedals are submodules, checked out at the commits its pinned
# superproject records. Nothing fetched is committed. Run again: an up-to-date checkout is left as it is.
# Usage: tools/fetch-upstream.sh [<folder>]
set -euo pipefail
cd "$(dirname "$0")/.."
. ./upstream.env
out=${1:-build/upstream}
mkdir -p "$out"

fetch() {   # repo, commit, dir, [submodules]
    local repo=$1 commit=$2 dir=$out/$3
    if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)" != "$commit" ]; then
        rm -rf "$dir"
        git init -q "$dir"
        git -C "$dir" fetch -q --depth 1 "https://github.com/$repo" "$commit"
        git -C "$dir" -c advice.detachedHead=false checkout -q FETCH_HEAD
    fi
    [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ] || { echo "$dir: HEAD is not $commit" >&2; exit 1; }
    if [ "${4:-}" = submodules ]; then
        git -C "$dir" submodule -q update --init --depth 1 --jobs 8
        git -C "$dir" submodule status | grep -q '^[-+U]' && { echo "$dir: a submodule is not at its pinned commit" >&2; exit 1; }
    fi
    echo "$repo @ ${commit:0:12}: $dir"
}
fetch "$GUITARIX_REPO" "$GUITARIX_COMMIT" guitarix
fetch "$GXPLUGINS_REPO" "$GXPLUGINS_COMMIT" gxplugins submodules

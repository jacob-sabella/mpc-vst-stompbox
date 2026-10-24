#!/usr/bin/env bash
# Whether guitarix or GxPlugins.lv2 has moved past the commit upstream.env pins (the head of each default branch;
# neither tags releases often enough to follow tags). If so, rewrites upstream.env for the new commits and prints a
# one-line summary ("guitarix 1234567..89abcde"); prints nothing otherwise. Needs git.
# Usage: tools/upstream-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."
. ./upstream.env
moved=()
check() {   # repo, variable name, current commit
    local head
    head=$(git ls-remote "https://github.com/$1" HEAD | cut -f1)
    [ -n "$head" ] || { echo "no HEAD for $1" >&2; exit 1; }
    if [ "$head" != "$3" ]; then
        sed -i "s/^$2=.*/$2=$head/" upstream.env
        moved+=("${1#*/} ${3:0:7}..${head:0:7}")
    fi
}
check "$GUITARIX_REPO" GUITARIX_COMMIT "$GUITARIX_COMMIT"
check "$GXPLUGINS_REPO" GXPLUGINS_COMMIT "$GXPLUGINS_COMMIT"
[ ${#moved[@]} -eq 0 ] || (IFS=,; echo "${moved[*]}")

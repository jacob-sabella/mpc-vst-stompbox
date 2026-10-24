#!/usr/bin/env bash
# Offline tests (x86, ASan/UBSan) before anything goes to a device:
#   1. tests/rack_test.cpp: the rack on its own (every model of every slot, knobs, switching, tail, state)
#   2. mpc-vst-plugins tools/test_port.sh: the generic wrapper's host test (instances, params, chunk)
# Fetches the pinned upstream sources and generates the model glue first (the fetch is skipped when already there).
# Needs an mpc-vst-plugins checkout (MPC_VST, default: next to this repo).
# Usage: tests/run.sh [plain]   (plain: -O2, no sanitizers; quicker, and its timings mean something)
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
export MPC_VST="${MPC_VST:-$here/../mpc-vst-plugins}"
[ -x "$MPC_VST/tools/test_port.sh" ] || { echo "need an mpc-vst-plugins checkout (MPC_VST)" >&2; exit 1; }
cd "$here"
flavour=${1:-asan}
[ -d build/upstream/guitarix ] || tools/fetch-upstream.sh
python3 tools/gen_models.py
tests/build_host.sh "$flavour"
out=build/host/$flavour
case "$flavour" in asan) F="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" ;; *) F="-O2" ;; esac
g++ $F -std=gnu++17 -Isrc -Ibuild/gen/build tests/rack_test.cpp $(cat "$out/objects") -lm -lpthread -o "$out/rack_test"
# upstream's chorus frees a new[] float buffer with plain delete: harmless for floats, and upstream is not patched here;
# known upstream leaks are listed in tests/lsan.supp
export ASAN_OPTIONS="${ASAN_OPTIONS:-alloc_dealloc_mismatch=0}" LSAN_OPTIONS="suppressions=$here/tests/lsan.supp"
"$out/rack_test"
"$MPC_VST/tools/test_port.sh" "$here/build/gen/vst.json"

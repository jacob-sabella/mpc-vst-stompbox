#!/usr/bin/env bash
# Builds the engine for this PC (x86, for the offline tests): every source build/gen/vst.json lists, in parallel,
# into build/host/<flavour>/. Run tools/fetch-upstream.sh and tools/gen_models.py first (tests/run.sh does).
# Usage: tests/build_host.sh [asan|plain]   -> prints the object files
set -euo pipefail
cd "$(dirname "$0")/.."
flavour=${1:-asan}
case "$flavour" in
    asan) F="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" ;;
    plain) F="-O2" ;;
    *) echo "usage: $0 [asan|plain]" >&2; exit 2 ;;
esac
out=build/host/$flavour
mkdir -p "$out"
cflags=$(python3 -c "import json; print(' '.join(json.load(open('build/gen/vst.json'))['build']['cflags']))")
python3 -c "import json; print('\n'.join(json.load(open('build/gen/vst.json'))['build']['sources']))" > "$out/sources"
mkdir -p build/gen/build && cp "${MPC_VST:-../mpc-vst-plugins}/wrapper/engine.h" build/gen/build/
# GCC 16 with the sanitizers at -O1 and up miscompiles guitarix's vibe (its LDR state comes out wrong and the output
# turns NaN); clang's ASan and MSan and every GCC build without sanitizers run it right, so that one unit is built
# plain at -O0 here
NOASAN="-g -O0"
[ "$flavour" = plain ] && NOASAN=$F
export F NOASAN cflags out
xargs -P "$(nproc)" -I{} sh -c '
    f={}; o="$out/$(echo "$f" | tr / _).o"
    flags=$F; case "$f" in */gxp_vibe.cpp) flags=$NOASAN ;; esac
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ src/host.h -nt "$o" ] || [ src/models.h -nt "$o" ]; then
        g++ $flags -fPIC -w $cflags -c "$f" -o "$o" || { echo "FAILED: $f" >&2; exit 1; }
    fi
    echo "$o"' < "$out/sources" > "$out/objects"

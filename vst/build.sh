#!/usr/bin/env bash
# Build Stompbox as an MPC OS VST2 insert effect with mpc-vst-plugins' generic port builder:
#   1. tools/fetch-upstream.sh   the upstream DSP at the commits pinned in upstream.env (skipped when present)
#   2. tools/gen_models.py       models.json -> build/gen/ (model glue, params.json, vst.json)
#   3. tools/build_port.sh       build/gen/vst.json -> build/gen/build/stompbox.so (+ aarch64/), skin, pluginlist entry
# Needs Docker (QEMU for arm32v7) and an mpc-vst-plugins checkout (MPC_VST, default: next to this repo).
# Usage: vst/build.sh [armv7|aarch64]   (default: every target in vst/vst.json)
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
MPC_VST="${MPC_VST:-$here/../mpc-vst-plugins}"
[ -x "$MPC_VST/tools/build_port.sh" ] || { echo "need an mpc-vst-plugins checkout (MPC_VST)" >&2; exit 1; }
cd "$here"
[ -d build/upstream/guitarix ] || tools/fetch-upstream.sh
python3 tools/gen_models.py
mkdir -p build/gen/build
cp "$MPC_VST/wrapper/engine.h" build/gen/build/   # the engine interface, first on the include path
exec "$MPC_VST/tools/build_port.sh" build/gen/vst.json "$@"

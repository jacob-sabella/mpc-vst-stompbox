#!/usr/bin/env bash
# CPU bench of whole rigs on an MPC device: mpc-vst-plugins' tools/bench.sh can't set parameters, so this builds
# a bench copy of the plugin (-DSTOMPBOX_BENCH: it starts from /tmp/stompbox_bench.state when that file exists)
# and benches it once per rig in tests/rigs/ (plus the default rig). Nothing is installed; MPC keeps running.
#   tools/bench_rig.sh <device-ip> [rig ...]      (rig: a tests/rigs/<name>.state; default: all of them)
# The device's architecture picks the build (armv7 Gen1, aarch64 Gen2). Needs Docker and MPC_VST, like vst/build.sh.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
MPC_VST="${MPC_VST:-$here/../mpc-vst-plugins}"
ip="$1"; shift
cd "$here"
rigs=("$@"); [ ${#rigs[@]} -gt 0 ] || rigs=($(cd tests/rigs && ls *.state | sed 's/\.state$//'))
case "$(ssh "root@$ip" uname -m)" in aarch64) target=aarch64; so=build/gen-bench/build/aarch64/stompbox.so ;;
                                      *) target=armv7; so=build/gen-bench/build/stompbox.so ;; esac
[ -d build/upstream/guitarix ] || tools/fetch-upstream.sh
python3 tools/gen_models.py --out build/gen-bench
python3 - <<'EOF'
import json
p = "build/gen-bench/vst.json"; c = json.load(open(p))
c["build"]["cflags"].append("-DSTOMPBOX_BENCH")
json.dump(c, open(p, "w"), indent=1)
EOF
mkdir -p build/gen-bench/build && cp "$MPC_VST/wrapper/engine.h" build/gen-bench/build/
"$MPC_VST/tools/build_port.sh" build/gen-bench/vst.json "$target"
for rig in default $(printf "%s\n" "${rigs[@]}" | grep -vx default); do
    echo "== rig: $rig"
    if [ "$rig" = default ]; then ssh "root@$ip" rm -f /tmp/stompbox_bench.state
    else ssh "root@$ip" "cat > /tmp/stompbox_bench.state" < "tests/rigs/$rig.state"; fi
    "$MPC_VST/tools/bench.sh" "$so" "$ip" -v 1 || true
done
ssh "root@$ip" rm -f /tmp/stompbox_bench.state

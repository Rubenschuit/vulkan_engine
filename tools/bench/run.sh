#!/usr/bin/env bash
# Repeat a benchmark N times with a cooldown between runs. Pass the resulting
# JSONs to noise_floor.py (spread) or compare.py (best-of-N).
#
#   tools/bench/run.sh <out-prefix> <repeats> <cooldown-sec> -- <VeApp bench args...>
#
# Example (5 cooled runs of a Bistro path):
#   tools/bench/run.sh bench/bistro_cpu 5 30 -- \
#       --bench-scene Bistro --bench-path tools/bench/paths/bistro/street.path \
#       --bench-warmup 200 --bench-frames 900
#
# Set VEAPP if the binary isn't at build/Release/VeApp[.exe]
set -euo pipefail

# Locate VeApp: honor $VEAPP, else try the usual Unix and Visual Studio (.exe)
# build layouts.
APP="${VEAPP:-}"
if [ -z "$APP" ]; then
    for cand in build/Release/VeApp build/Release/VeApp.exe build/release/VeApp.exe; do
        [ -x "$cand" ] && { APP="$cand"; break; }
    done
fi

prefix="$1"; repeats="$2"; cooldown="$3"; shift 3
[ "$1" = "--" ] && shift

[ -n "$APP" ] && [ -x "$APP" ] || { echo "error: VeApp not found (set VEAPP or build Release)" >&2; exit 1; }

failures=0
for n in $(seq 1 "$repeats"); do
    out="${prefix}_${n}.json"
    echo "[run.sh] run $n/$repeats -> $out"
    set +e
    "$APP" "$@" --bench-stats "$out" 2>&1 | grep -E "\[bench\]"
    status=${PIPESTATUS[0]}
    set -e
    if [ "$status" -ne 0 ]; then
        echo "[run.sh] run $n FAILED (VeApp exit $status), not a usable sample; see log above" >&2
        failures=$((failures + 1))
    fi
    if [ "$n" -lt "$repeats" ]; then
        echo "[run.sh] cooldown ${cooldown}s"
        sleep "$cooldown"
    fi
done

if [ "$failures" -ne 0 ]; then
    echo "[run.sh] done with ${failures}/${repeats} failed run(s): ${prefix}_1..${repeats}.json" >&2
    exit 1
fi
echo "[run.sh] done: ${prefix}_1..${repeats}.json"
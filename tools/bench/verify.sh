#!/usr/bin/env bash
# Verify the current build against the existing per-machine baselines: capture
# candidate runs, gate timings/counters (compare.py) + goldens, print a verdict.
# Does not modify baselines/goldens.
#
#   tools/bench/verify.sh <machine-id> [--scenes "Bistro Sponza Simple"]
#       [--backends "cpu gpu meshlet"] [--repeats N] [--cooldown S] [--no-goldens]
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
PYTHON="${PYTHON:-python3}"
[ $# -ge 1 ] || { echo "usage: verify.sh <machine-id> [options]"; exit 1; }
machine="$1"; shift
base="bench/baselines/$machine"
[ -d "$base" ] || { echo "error: no baselines at $base (capture with bench.sh + promote.py first)"; exit 1; }

scenes="Bistro Sponza Simple"; backends="cpu gpu meshlet"; repeats=3; cooldown=30; goldens=1
while [ $# -gt 0 ]; do
    case "$1" in
        --scenes)   scenes="$2"; shift 2;;
        --backends) backends="$2"; shift 2;;
        --repeats)  repeats="$2"; shift 2;;
        --cooldown) cooldown="$2"; shift 2;;
        --no-goldens) goldens=0; shift;;
        *) echo "unknown option: $1"; exit 1;;
    esac
done

cand="bench/verify/$machine"; rm -rf "$cand"; mkdir -p "$cand"
status=0

first_scene=1
for scene in $scenes; do
    [ "$first_scene" -eq 0 ] && { echo "[verify] cooldown ${cooldown}s"; sleep "$cooldown"; }
    first_scene=0
    "$here/bench.sh" "$scene" --backends "$backends" --repeats "$repeats" \
        --cooldown "$cooldown" --out "$cand/$scene" \
        || { echo "[verify] capture failed for $scene, aborting" >&2; exit 1; }
done

echo ""; echo "======== TIMING + COUNTER GATE (candidate vs baseline) ========"
for scene in $scenes; do
    for be in $backends; do
        for noisef in "$base/$scene"/*_"$be".noise.json; do
            [ -f "$noisef" ] || continue
            fn="$(basename "$noisef")"; pose="${fn%_${be}.noise.json}"
            echo "---- $scene / $pose / $be ----"
            "$PYTHON" "$here/compare.py" \
                --baseline "$base/$scene/${pose}_${be}"_*.json \
                --candidate "$cand/$scene/${pose}_${be}"_*.json \
                --noise "$noisef" || status=1
        done
    done
done

if [ "$goldens" -eq 1 ]; then
    echo ""; echo "======== GOLDEN IMAGE GATE ========"
    "$here/goldens.sh" check "$machine" || status=1
fi

echo ""
[ "$status" -eq 0 ] && echo "VERIFY: clean" || echo "VERIFY: changes detected — review above (regression, improvement, or intended workload change)"
exit "$status"
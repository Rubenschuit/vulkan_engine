#!/usr/bin/env bash
# Capture or check per-scene golden images (visual regression).
#   tools/bench/goldens.sh capture <machine-id>   # render + store references
#   tools/bench/goldens.sh check   <machine-id>   # render + diff vs stored
# Goldens are machine-specific, so they live under the gitignored
# bench/goldens/<machine-id>/. check writes <scene>_diff.png for any mismatch.
set -euo pipefail

# Locate VeApp: first $VEAPP, else try the usual Unix and Visual Studio (.exe)
# build layouts.
APP="${VEAPP:-}"
if [ -z "$APP" ]; then
    for cand in build/Release/VeApp build/Release/VeApp.exe build/release/VeApp.exe; do
        [ -x "$cand" ] && { APP="$cand"; break; }
    done
fi
PYTHON="${PYTHON:-python3}"
[ $# -eq 2 ] || { echo "usage: goldens.sh <capture|check> <machine-id>"; exit 1; }
mode="$1"; dir="bench/goldens/$2"
here="$(cd "$(dirname "$0")" && pwd)"

poses=(
    "Bistro_indoor:-13.37,-15.09,3.78:-6.71,-22.54,4.13"
    "Bistro_outdoor:-46.54,-26.91,3.79:-36.81,-24.76,4.61"
    "Sponza:-12.61,2.51,-46.73:-3.02,-0.24,-45.95"
    "Simple:20,20,20:0,0,5"
)

[ -n "$APP" ] && [ -x "$APP" ] || { echo "error: VeApp not found (set VEAPP or build Release)"; exit 1; }
mkdir -p "$dir"
status=0
for entry in "${poses[@]}"; do
    label="${entry%%:*}"; cam="${entry#*:}"
    scene="${label%%_*}"
    shot="$dir/${label}.candidate.png"
    # Remove any stale candidate so a failed render can't pass against old pixels.
    rm -f "$shot"
    set +e
    "$APP" --bench-scene "$scene" --bench-warmup 200 --bench-frames 6 \
        --bench-camera "$cam" --bench-screenshot "$shot" 2>&1 | grep -E "\[bench\]" >/dev/null
    app_status=${PIPESTATUS[0]}
    set -e
    if [ "$app_status" -ne 0 ]; then
        echo "[goldens] $label render FAILED (VeApp exit $app_status)" >&2
        status=1; continue
    fi

    if [ "$mode" = "capture" ]; then
        mv "$shot" "$dir/${label}.png"
        echo "[goldens] captured $label -> $dir/${label}.png"
    else
        golden="$dir/${label}.png"
        [ -f "$golden" ] || { echo "[goldens] no golden for $label (capture first)"; status=1; continue; }
        rm -f "$dir/${label}_diff.png"
        "$PYTHON" "$here/imgdiff.py" "$golden" "$shot" --diff "$dir/${label}_diff.png" || status=1
        rm -f "$shot"
    fi
done
exit "$status"

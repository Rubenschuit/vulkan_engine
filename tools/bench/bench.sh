#!/usr/bin/env bash
# Capture the benchmark matrix for a scene: every pose file in
# tools/bench/paths/<scene>/ x every culling backend, N repeats each.
#
#   tools/bench/bench.sh <scene> [options]
#     --backends "cpu gpu meshlet"   (default)
#     --repeats N                    (default 3; feeds compare.py best-of-N)
#     --cooldown S                   (default 30; sleep between runs)
#     --res WxH                      (default 1920x1080)
#     --warmup N / --frames N        (default 200 / 900)
#     --out DIR                      (default bench/<scene>)
#
# Writes DIR/<pose>_<backend>_<n>.json. Compare a candidate build against these
# with compare.py, or characterize spread with noise_floor.py.
set -euo pipefail

# Locate VeApp: first $VEAPP, else try the usual Unix and Visual Studio (.exe)
# build layouts.
APP="${VEAPP:-}"
if [ -z "$APP" ]; then
    for cand in build/Release/VeApp build/Release/VeApp.exe build/release/VeApp.exe; do
        [ -x "$cand" ] && { APP="$cand"; break; }
    done
fi
[ $# -ge 1 ] || { echo "usage: bench.sh <scene> [options]"; exit 1; }
scene="$1"; shift

backends="cpu gpu meshlet"
repeats=3; cooldown=30; res=1920x1080; warmup=200; frames=900
paths_dir=""; out=""
while [ $# -gt 0 ]; do
    case "$1" in
        --backends) backends="$2"; shift 2;;
        --repeats)  repeats="$2"; shift 2;;
        --cooldown) cooldown="$2"; shift 2;;
        --res)      res="$2"; shift 2;;
        --warmup)   warmup="$2"; shift 2;;
        --frames)   frames="$2"; shift 2;;
        --paths)    paths_dir="$2"; shift 2;;
        --out)      out="$2"; shift 2;;
        *) echo "unknown option: $1"; exit 1;;
    esac
done

scene_lc=$(printf '%s' "$scene" | tr '[:upper:]' '[:lower:]')
paths_dir="${paths_dir:-tools/bench/paths/${scene_lc}}"
out="${out:-bench/${scene}}"
[ -n "$APP" ] && [ -x "$APP" ] || { echo "error: VeApp not found (set VEAPP or build Release)"; exit 1; }
[ -d "$paths_dir" ] || { echo "error: no pose dir $paths_dir"; exit 1; }
shopt -s nullglob
path_files=("$paths_dir"/*.path)
[ ${#path_files[@]} -gt 0 ] || { echo "error: no .path files in $paths_dir"; exit 1; }
mkdir -p "$out"

first=1
for path_file in "${path_files[@]}"; do
    pose=$(basename "$path_file" .path)
    for backend in $backends; do
        for n in $(seq 1 "$repeats"); do
            [ "$first" -eq 0 ] && { echo "[bench] cooldown ${cooldown}s"; sleep "$cooldown"; }
            first=0
            json="${out}/${pose}_${backend}_${n}.json"
            echo "[bench] ${scene}/${pose} ${backend} run ${n}/${repeats} -> ${json}"
            set +e
            "$APP" --bench-scene "$scene" --bench-path "$path_file" \
                --bench-culling "$backend" --bench-res "$res" \
                --bench-warmup "$warmup" --bench-frames "$frames" \
                --bench-stats "$json" 2>&1 | grep -E "\[bench\]"
            app_status=${PIPESTATUS[0]}
            set -e
            if [ "$app_status" -ne 0 ]; then
                echo "error: VeApp exited $app_status on ${scene}/${pose} ${backend} run ${n}; aborting capture" >&2
                exit "$app_status"
            fi
        done
    done
done
echo "[bench] matrix done -> ${out}/"
